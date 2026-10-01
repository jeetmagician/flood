// =====================================================================
//  BENCH TEST  -  RECEIVER  (ESP32-C3)
//  ESP32-C3 + RA-02 / SX1278 LoRa at 433 MHz.  No WiFi, no MQTT, no SD.
//
//  Receives level packets from the classic-ESP32 sender, prints them,
//  and drives one LED:
//
//    one short flash (80 ms)   = packet received, level below the mark
//    LED ON, steady            = ALERT - level at or above the mark
//    three fast flashes        = packet arrived but the CRC failed
//    LED off, nothing printed  = nothing being received
//
//  ---------------------------------------------------------------
//  ARDUINO IDE SETTINGS  (these matter more than usual on the C3)
//    Board ................ "ESP32C3 Dev Module"
//    USB CDC On Boot ...... ENABLED      <-- or Serial prints nowhere
//    Upload Speed ......... 921600
//  The C3 talks to the PC over native USB, not a USB-serial chip. With
//  USB CDC On Boot disabled, Serial goes to UART0 on GPIO20/21 instead
//  and the monitor stays blank while the LED works perfectly.
//  ---------------------------------------------------------------
// =====================================================================

#include <SPI.h>
#include <LoRa.h>

// ---------- pins: ESP32-C3 --------------------------------------------
// Go by GPIO NUMBER, not by position - C3 boards vary physically.
// Avoided on purpose:
//   GPIO11-17  wired to the internal flash. Using them bricks the boot.
//   GPIO18/19  native USB D-/D+.
//   GPIO20/21  UART0.
//   GPIO2/8/9  strapping pins, read at power-up.
#define LORA_SCK    4
#define LORA_MISO   5
#define LORA_MOSI   6
#define LORA_SS     7
#define LORA_RST    3
#define LORA_DIO0  10

#define PIN_LED     1          // external LED + 330 ohm to GND, active HIGH
#define PIN_LED_ONBOARD 8      // SuperMini's blue LED, wired ACTIVE LOW
#define HAS_ONBOARD_LED 1      // set 0 if your board has none, or an RGB one

// ---------- radio: identical to the sender or nothing arrives ---------
#define LORA_FREQ  433000000L      // 433.0 MHz - SX1278 / RA-02
#define LORA_SF    9
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

// ---------- the alert threshold ----------------------------------------
// The sender reports LEVEL, not distance:   level = DATUM_MM - distance
// so level RISES as the target gets CLOSER to the transducer, exactly as
// river stage rises as water comes up toward a bridge-mounted sensor.
//
// With the sender's DATUM_MM = 2000:
//     hand 1500 mm away  ->  level  500 mm   quiet
//     hand  500 mm away  ->  level 1500 mm   ALERT
#define ALERT_ON_MM   1500
#define ALERT_OFF_MM  1400       // hysteresis: must fall below this to clear
#define ALERT_HOLD_MS 6000       // clear the alert if nothing is heard

unsigned long ledOffAt  = 0;
unsigned long lastHeard = 0;
unsigned long lastWarn  = 0;
uint32_t      goodCount = 0, badCount = 0;
bool          alert     = false;

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
  digitalWrite(PIN_LED, on ? HIGH : LOW);          // external: active HIGH
#if HAS_ONBOARD_LED
  digitalWrite(PIN_LED_ONBOARD, on ? LOW : HIGH);  // onboard:  active LOW
#endif
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
  delay(400);                    // native USB needs a moment to enumerate

  pinMode(PIN_LED, OUTPUT);
#if HAS_ONBOARD_LED
  pinMode(PIN_LED_ONBOARD, OUTPUT);
#endif
  ledWrite(false);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
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

  Serial.println("RECEIVER ready (ESP32-C3). Listening on 433.0 MHz.");
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
  if (!alert) flashGood();

  Serial.print(alert ? "ALERT  " : "ok     ");
  Serial.print("node ");    Serial.print(node);
  Serial.print("  seq ");   Serial.print(seq);
  Serial.print("  level "); Serial.print(level);
  Serial.print(" mm  RSSI "); Serial.print(LoRa.packetRssi());
  Serial.print(" dBm  SNR "); Serial.print(LoRa.packetSnr(), 1);
  Serial.print(" dB   good "); Serial.print(goodCount);
  Serial.print(" / bad ");     Serial.println(badCount);
}
