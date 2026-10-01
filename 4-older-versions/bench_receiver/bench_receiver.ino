// =====================================================================
//  BENCH TEST  —  RECEIVER
//  ESP32 + SX1276 LoRa.  No WiFi, no MQTT, no SD card.
//  Receives level packets, prints them, and drives one LED:
//
//    one short flash (80 ms)   = packet received, level below the alert mark
//    LED ON, steady            = ALERT - level at or above the alert mark
//    three fast flashes        = packet arrived but the CRC failed
//    LED off, nothing printed  = nothing being received
// =====================================================================

#include <SPI.h>
#include <LoRa.h>

#define PIN_LED       2          // the DevKit's own blue LED
#define PIN_LED_EXT  25          // optional external LED + 330 ohm to GND

// ---------- the alert threshold ----------------------------------------
// The sender reports LEVEL, not distance:   level = DATUM_MM - distance
// so level RISES as the target gets CLOSER to the transducer, exactly as
// river stage rises as the water comes up toward a bridge-mounted sensor.
//
// With the sender's DATUM_MM = 2000:
//     hand 1500 mm away  ->  level  500 mm   quiet
//     hand  500 mm away  ->  level 1500 mm   ALERT
//
// ALERT_ON_MM  - alert turns on at or above this level
// ALERT_OFF_MM - and only clears once the level falls back below this.
// The gap between them is hysteresis: without it a level hovering on the
// mark switches the alarm on and off several times a second.
#define ALERT_ON_MM   1500
#define ALERT_OFF_MM  1400
#define ALERT_HOLD_MS 6000       // clear the alert if nothing is heard for this long

#define LORA_SS       5
#define LORA_RST     14
#define LORA_DIO0    26

// Identical to the sender. Change one of these and you receive nothing.
#define LORA_FREQ  433000000L      // 433.0 MHz - SX1278 / RA-02
#define LORA_SF    9
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

unsigned long ledOffAt  = 0;     // non-blocking flash timer
unsigned long lastHeard = 0;
unsigned long lastWarn  = 0;
uint32_t      goodCount = 0, badCount = 0;
bool          alert     = false; // true while the level is above the mark

uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

void ledWrite(bool on) {
  digitalWrite(PIN_LED,     on ? HIGH : LOW);
  digitalWrite(PIN_LED_EXT, on ? HIGH : LOW);
}

void flashGood() {               // one short flash, does not block reception
  ledWrite(true);
  ledOffAt = millis() + 80;
}

void flashBad() {                // three fast flashes
  for (uint8_t i = 0; i < 3; i++) {
    ledWrite(true);  delay(50);
    ledWrite(false); delay(50);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_LED,     OUTPUT);
  pinMode(PIN_LED_EXT, OUTPUT);
  ledWrite(false);

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
  LoRa.receive();                // park in continuous receive

  Serial.println("RECEIVER ready. Listening.");
  lastHeard = millis();
}

void loop() {
  // while the alert is on the LED stays lit; otherwise end the short flash
  if (!alert && ledOffAt && millis() > ledOffAt) { ledWrite(false); ledOffAt = 0; }

  // a stuck alert is worse than no alert: clear it if the sender goes quiet
  if (alert && millis() - lastHeard > ALERT_HOLD_MS) {
    alert = false;
    ledWrite(false);
    Serial.println("alert cleared - no packets, level unknown");
  }

  // say something if the link has gone quiet
  if (millis() - lastHeard > 10000 && millis() - lastWarn > 10000) {
    lastWarn = millis();
    Serial.println("...nothing for 10 s. Sender powered? Same radio settings?");
  }

  int sz = LoRa.parsePacket();
  if (sz == 0) return;

  if (sz != 13) {                              // not one of ours
    Serial.print("ignored a packet of "); Serial.print(sz); Serial.println(" bytes");
    return;
  }

  uint8_t p[13];
  for (int i = 0; i < 13; i++) p[i] = LoRa.read();

  uint16_t got = ((uint16_t)p[11] << 8) | p[12];
  if (got != crc16(p, 11)) {
    badCount++;
    Serial.println("CRC FAILED - packet corrupted in the air");
    flashBad();
    return;
  }

  uint8_t  node  = p[1];
  uint16_t seq   = ((uint16_t)p[2] << 8) | p[3];
  int16_t  level = (int16_t)(((uint16_t)p[4] << 8) | p[5]);

  goodCount++;
  lastHeard = millis();

  // ---------- the alert decision, with hysteresis ----------------------
  if (!alert && level >= ALERT_ON_MM) {
    alert = true;
    ledWrite(true);                       // solid on, stays on
    Serial.println("*** ALERT - level above the mark ***");
  } else if (alert && level < ALERT_OFF_MM) {
    alert = false;
    ledWrite(false);
    Serial.println("alert cleared - level back below the mark");
  }
  if (!alert) flashGood();                // quiet: just blink to show a packet came in

  Serial.print(alert ? "ALERT  " : "ok     ");
  Serial.print("node ");    Serial.print(node);
  Serial.print("  seq ");   Serial.print(seq);
  Serial.print("  level "); Serial.print(level);
  Serial.print(" mm  RSSI "); Serial.print(LoRa.packetRssi());
  Serial.print(" dBm  SNR "); Serial.print(LoRa.packetSnr(), 1);
  Serial.print(" dB   good "); Serial.print(goodCount);
  Serial.print(" / bad ");     Serial.println(badCount);
}
