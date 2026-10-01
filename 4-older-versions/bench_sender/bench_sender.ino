// =====================================================================
//  BENCH TEST  —  SENDER
//  ESP32 + JSN-SR04T ultrasonic + SX1276 LoRa.
//  No rain gauge, no temperature probe, no battery monitor, no sleep.
//  Measures distance every 2 s, transmits it, blinks its own LED.
//
//  The packet is the same 13-byte format the field system uses, with the
//  rain and battery fields set to zero - so the real gateway firmware
//  will decode these packets unchanged when you move on.
// =====================================================================

#include <SPI.h>
#include <LoRa.h>

#define NODE_ID         1
#define DATUM_MM     2000        // bench value: pretend the sensor sits 2 m up
#define TX_INTERVAL  2000        // milliseconds between transmissions
#define FIXED_TEMP_C   25.0f     // no DS18B20 on the bench

#define PIN_TRIG   17
#define PIN_ECHO   16
#define PIN_LED     2            // the DevKit's own blue LED

#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26

// These five must be identical on the receiver or nothing arrives.
// RA-02 carries an SX1278, which tunes 137-525 MHz. It CANNOT do 865 MHz:
// set that and the radio still initialises, it just never hears anything.
#define LORA_FREQ  433000000L      // 433.0 MHz - SX1278 / RA-02
#define LORA_SF    9
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

uint16_t seq = 0;

uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

uint32_t pingOnce() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  return pulseIn(PIN_ECHO, HIGH, 40000UL);      // 40 ms ceiling, about 6.8 m
}

int cmpU32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

// Median of 9 pings. Returns millimetres, or -1 if the reading is no good.
int16_t measureDistanceMM() {
  uint32_t s[9];
  uint8_t  n = 0;
  for (uint8_t i = 0; i < 9; i++) {
    uint32_t t = pingOnce();
    if (t > 0) s[n++] = t;
    delay(40);
  }
  if (n < 5) return -1;
  qsort(s, n, sizeof(uint32_t), cmpU32);

  float c  = 331.4f + 0.606f * FIXED_TEMP_C;    // speed of sound, m/s
  float mm = (s[n / 2] * c) / 2000.0f;
  if (mm < 250.0f || mm > 4500.0f) return -1;   // blind zone / out of range
  return (int16_t)(mm + 0.5f);
}

void sendPacket(int16_t levelMM) {
  uint8_t p[13];
  p[0]  = 1;                       // protocol version
  p[1]  = NODE_ID;
  p[2]  = seq >> 8;      p[3]  = seq & 0xFF;
  p[4]  = levelMM >> 8;  p[5]  = levelMM & 0xFF;
  p[6]  = 0;             p[7]  = 0;            // rain tips - none on the bench
  p[8]  = 0;             p[9]  = 0;            // battery mV - USB powered
  p[10] = (int8_t)FIXED_TEMP_C;
  uint16_t c = crc16(p, 11);
  p[11] = c >> 8;        p[12] = c & 0xFF;

  LoRa.beginPacket();
  LoRa.write(p, 13);
  LoRa.endPacket();                // blocks until the packet is on the air
  seq++;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED,  OUTPUT);
  digitalWrite(PIN_LED, LOW);

  SPI.begin(18, 19, 23, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  while (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init failed - check wiring, and fit a 433 MHz antenna");
    delay(2000);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC);
  LoRa.setTxPower(14);

  Serial.println("SENDER ready. Transmitting every 2 s.");
}

void loop() {
  int16_t dist  = measureDistanceMM();
  int16_t level = (dist < 0) ? -1 : (int16_t)(DATUM_MM - dist);

  Serial.print("seq ");   Serial.print(seq);
  Serial.print("  dist "); Serial.print(dist);
  Serial.print(" mm   level "); Serial.print(level);
  Serial.println(" mm");

  sendPacket(level);

  digitalWrite(PIN_LED, HIGH);     // one flash per transmission
  delay(60);
  digitalWrite(PIN_LED, LOW);

  delay(TX_INTERVAL);
}
