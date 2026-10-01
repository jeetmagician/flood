// =====================================================================
//  TANK TEST  -  SENDER   (classic ESP32-WROOM-32)
//  JSN-SR04T ultrasonic + RA-02 / SX1278 LoRa 433 MHz
//
//  Sensor looks straight DOWN from the top of a 3 ft column.
//
//      sensor face  ---------------------  gap = 3 ft, level = 0 ft
//                        (air)
//      alert line   - - - - - - - - - - -  gap = 1 ft   <-- LED blinks
//                        (air)
//      water surface ~~~~~~~~~~~~~~~~~~~~
//                       (water)
//      empty bed    =====================  3 ft below the sensor
//
//  gap   = distance from the sensor face down to the water  (falls as water rises)
//  level = depth of water = DEPTH_MM - gap                  (rises as water rises)
//
//  Transmits both, ten times a second is pointless on LoRa, so twice a
//  second, which still looks live to the eye.
// =====================================================================

#include <SPI.h>
#include <LoRa.h>

// ---------- geometry: measure these on your rig -----------------------
#define DEPTH_MM        914      // 3 ft 0 in - sensor face to the empty bed
#define ALERT_GAP_MM    305      // 1 ft 0 in - alarm when the gap falls to this
#define CLEAR_GAP_MM    355      // and clears only above this (hysteresis)

#define BLIND_MM        250      // JSN-SR04T cannot see closer than this
#define MAX_RANGE_MM   4500

#define NODE_ID           1
#define TX_INTERVAL_MS  500

// ---------- pins: unchanged from the bench rig ------------------------
#define PIN_TRIG   17
#define PIN_ECHO   16
#define PIN_LED     2
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26

// ---------- radio: SF7 here, not SF9 ----------------------------------
// A 13-byte packet at SF9 is 165 ms in the air. Twice a second that is a
// 33% duty cycle, which is antisocial and probably not legal. SF7 puts the
// same packet on the air in 46 ms - under 10% - at the cost of range you
// do not need on a bench. The RECEIVER MUST USE SF7 TOO.
#define LORA_FREQ  433000000L
#define LORA_SF    7
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

// ---------- status bits carried in the packet -------------------------
#define ST_NO_ECHO   0x01        // nothing came back at all
#define ST_SUB_BLIND 0x02        // water is inside the blind zone - too high to measure
#define ST_ALERT     0x04        // sender's own view of the alert

uint16_t seq   = 0;
bool     alert = false;

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
  return pulseIn(PIN_ECHO, HIGH, 30000UL);    // 30 ms ceiling, about 5 m
}

int cmpU32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

// Median of 5 - enough to reject a wave crest, fast enough for 2 Hz.
// Returns the gap in mm, or one of these two sentinels:
//   -1  no echo at all
//   -2  echo, but closer than the blind zone
int16_t measureGapMM() {
  uint32_t s[5];
  uint8_t  n = 0;
  for (uint8_t i = 0; i < 5; i++) {
    uint32_t t = pingOnce();
    if (t > 0) s[n++] = t;
    delay(25);
  }
  if (n < 3) return -1;
  qsort(s, n, sizeof(uint32_t), cmpU32);

  float mm = (s[n / 2] * 343.0f) / 2000.0f;   // 343 m/s at about 20 C
  if (mm > MAX_RANGE_MM) return -1;
  if (mm < BLIND_MM)     return -2;
  return (int16_t)(mm + 0.5f);
}

void sendPacket(int16_t gap, int16_t level, uint8_t status) {
  uint8_t p[13];
  p[0]  = 2;                       // protocol 2 = tank test
  p[1]  = NODE_ID;
  p[2]  = seq >> 8;      p[3]  = seq & 0xFF;
  p[4]  = level >> 8;    p[5]  = level & 0xFF;
  p[6]  = status;        p[7]  = 0;
  p[8]  = gap >> 8;      p[9]  = gap & 0xFF;
  p[10] = 0;
  uint16_t c = crc16(p, 11);
  p[11] = c >> 8;        p[12] = c & 0xFF;

  LoRa.beginPacket();
  LoRa.write(p, 13);
  LoRa.endPacket();
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

  Serial.println("SENDER ready. Column depth 914 mm (3 ft), alert at 305 mm (1 ft).");
}

void loop() {
  // Measuring takes about 125 ms, so a plain delay(500) would give a 1.6 Hz
  // cycle, not 2 Hz. Pace from the top of the loop instead.
  static unsigned long nextTx = 0;
  while (millis() < nextTx) { delay(2); }
  nextTx = millis() + TX_INTERVAL_MS;

  int16_t gap    = measureGapMM();
  uint8_t status = 0;
  int16_t level;

  if (gap == -1) {
    // No echo. Could be a soft or angled surface - or the water is so high
    // the return is lost. Report the last-known-bad and say so.
    status |= ST_NO_ECHO;
    gap = -1; level = -1;
  } else if (gap == -2) {
    // Inside the blind zone: the water is ABOVE the alert line and rising.
    // Treat it as the worst case, not as an error.
    status |= ST_SUB_BLIND;
    gap   = BLIND_MM;
    level = DEPTH_MM - BLIND_MM;
  } else {
    if (gap > DEPTH_MM) gap = DEPTH_MM;       // empty column, or floor echo
    level = DEPTH_MM - gap;
  }

  // ---------- alert decision, with hysteresis -------------------------
  if (status & ST_SUB_BLIND) {
    alert = true;                              // cannot see it, assume the worst
  } else if (!(status & ST_NO_ECHO)) {
    if (!alert && gap <= ALERT_GAP_MM)      alert = true;
    else if (alert && gap >= CLEAR_GAP_MM)  alert = false;
  }
  if (alert) status |= ST_ALERT;

  sendPacket(gap, level, status);

  digitalWrite(PIN_LED, alert ? HIGH : LOW);

  Serial.print(alert ? "ALERT " : "ok    ");
  Serial.print("seq ");  Serial.print(seq);
  if (gap < 0) {
    Serial.println("   no echo");
  } else {
    Serial.print("   gap ");   Serial.print(gap);
    Serial.print(" mm (");     Serial.print(gap / 304.8f, 2);
    Serial.print(" ft)   level "); Serial.print(level);
    Serial.print(" mm (");     Serial.print(level / 304.8f, 2);
    Serial.print(" ft)");
    if (status & ST_SUB_BLIND) Serial.print("   [inside blind zone]");
    Serial.println();
  }

}
