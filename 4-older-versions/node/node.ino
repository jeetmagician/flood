// =====================================================================
//  FLOOD NODE  —  SENDER  (transmitter)
//  ESP32-WROOM-32 + JSN-SR04T v2.0 + tipping bucket + DS18B20 + SX1276
//  Arduino IDE.  One file.  Every cycle ends in deep sleep.
// =====================================================================

#include <SPI.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "driver/rtc_io.h"

// ---------- per-node configuration: change these three per station ----
#define NODE_ID        1          // unique per node, 1..254
#define DATUM_MM       6250       // sensor face above datum, measured on site
#define MM_PER_TIP     0.2f       // from the rain gauge calibration (page 10)

#define SLEEP_SECONDS  300        // 5 minutes between transmissions

// ---------- pin map (matches the tables on page 5) --------------------
#define PIN_TRIG   17             // TX2 on the silkscreen
#define PIN_ECHO   16             // RX2 on the silkscreen
#define PIN_RAIN   GPIO_NUM_33    // RTC pin - can wake the chip
#define PIN_TEMP    4
#define PIN_VBAT   35             // input-only, ADC1
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26

// ---------- radio: must match the gateway exactly ---------------------
#define LORA_FREQ  865100000L     // IN865 licence-free band
#define LORA_SF    9
#define LORA_BW    125E3
#define LORA_SYNC  0x34

// ---------- these survive deep sleep ----------------------------------
RTC_DATA_ATTR uint16_t rainTips = 0;
RTC_DATA_ATTR uint16_t seq      = 0;

OneWire oneWire(PIN_TEMP);
DallasTemperature tempSensor(&oneWire);

// ---------- CRC16-CCITT. The gateway runs the identical function ------
uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// ---------- one ultrasonic ping: returns echo width in microseconds ---
uint32_t pingOnce() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  return pulseIn(PIN_ECHO, HIGH, 40000UL);   // 40 ms ceiling = about 6.8 m
}

int cmpU32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

// ---------- median of 9 pings, corrected for air temperature ----------
// A single ping lands on whatever the surface was doing at that instant.
// The median throws out both the wave crest and the spray dropout;
// a mean would not.
int16_t measureDistanceMM(float tempC) {
  uint32_t s[9];
  uint8_t  n = 0;
  for (uint8_t i = 0; i < 9; i++) {
    uint32_t t = pingOnce();
    if (t > 0) s[n++] = t;
    delay(60);                        // let the echoes die away
  }
  if (n < 5) return -1;               // too few valid returns
  qsort(s, n, sizeof(uint32_t), cmpU32);
  uint32_t median = s[n / 2];

  float c  = 331.4f + 0.606f * tempC;         // speed of sound, m/s
  float mm = (median * c) / 2000.0f;          // us x m/s / 2  ->  mm
  if (mm < 250.0f || mm > 4500.0f) return -1; // blind zone / out of range
  return (int16_t)(mm + 0.5f);
}

// ---------- battery, through the 100k/100k divider on GPIO35 ----------
uint16_t readBatteryMV() {
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);
  uint32_t acc = 0;
  for (uint8_t i = 0; i < 16; i++) { acc += analogReadMilliVolts(PIN_VBAT); delay(2); }
  return (uint16_t)((acc / 16) * 2);          // the divider halved it
}

// ---------- build and send the 13-byte packet (format on page 2) ------
void sendPacket(int16_t levelMM, int8_t tempC, uint16_t battMV) {
  uint8_t p[13];
  p[0]  = 1;                                  // protocol version
  p[1]  = NODE_ID;
  p[2]  = seq >> 8;         p[3]  = seq & 0xFF;
  p[4]  = levelMM >> 8;     p[5]  = levelMM & 0xFF;
  p[6]  = rainTips >> 8;    p[7]  = rainTips & 0xFF;
  p[8]  = battMV >> 8;      p[9]  = battMV & 0xFF;
  p[10] = (uint8_t)tempC;
  uint16_t c = crc16(p, 11);
  p[11] = c >> 8;           p[12] = c & 0xFF;

  LoRa.beginPacket();
  LoRa.write(p, 13);
  LoRa.endPacket();                           // blocks until sent
  seq++;
}

// ---------- arm both wake sources and sleep ---------------------------
void goToSleep() {
  LoRa.sleep();
  rtc_gpio_pullup_en(PIN_RAIN);               // hold the line high in sleep
  esp_sleep_enable_ext0_wakeup(PIN_RAIN, 0);  // wake when a tip pulls it low
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(33, INPUT_PULLUP);                  // no external pull-up fitted

  esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();

  // --- a rain tip: count it and go straight back to sleep -------------
  if (why == ESP_SLEEP_WAKEUP_EXT0) {
    rainTips++;
    delay(150);                               // let the contact settle
    while (digitalRead(33) == LOW) delay(10); // wait for the bucket to finish
    delay(100);
    goToSleep();
  }

  // --- timer wake, or first boot: measure and transmit ----------------
  tempSensor.begin();
  tempSensor.requestTemperatures();
  float tC = tempSensor.getTempCByIndex(0);
  if (tC < -40 || tC > 80) tC = 25.0;         // probe missing -> assume 25 C

  int16_t dist  = measureDistanceMM(tC);
  int16_t level = (dist < 0) ? -1 : (int16_t)(DATUM_MM - dist);

  SPI.begin(18, 19, 23, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (LoRa.begin(LORA_FREQ)) {
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(5);
    LoRa.setSyncWord(LORA_SYNC);
    LoRa.setTxPower(14);
    sendPacket(level, (int8_t)tC, readBatteryMV());
  }

  rainTips = 0;              // counter reported, start the next interval
  goToSleep();
}

void loop() { }              // never reached - every cycle ends in sleep
