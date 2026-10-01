// =====================================================================
//  STEP 1 TEST  -  is the ESP32-C3 alive, and does its WiFi work?
//  Nothing else: no LoRa, no SPI, no sensor. Flash this FIRST.
//
//  WHAT YOU SHOULD SEE
//    - the LED blinks steadily, about twice a second, forever
//    - a WiFi network called  C3-TEST  appears within ~10 seconds
//    - joining it (password flood1234) and opening http://192.168.4.1
//      shows a page saying the board is alive
//
//  IF THE LED NEVER BLINKS: the sketch is not running. Wrong board
//  selected, upload did not take, or the board is sitting in bootloader
//  mode - press RESET once after uploading.
//
//  IF THE LED BLINKS BUT NO WIFI APPEARS: the WiFi side is the problem,
//  not your wiring. Look at the serial monitor.
//
//  ARDUINO IDE:  Board "ESP32C3 Dev Module",  USB CDC On Boot: ENABLED
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>

#define PIN_LED          1     // external LED + 330 ohm to GND, active HIGH
#define PIN_LED_ONBOARD  8     // SuperMini blue LED, active LOW

WebServer server(80);
unsigned long t = 0;
bool on = false;

void ledWrite(bool s) {
  digitalWrite(PIN_LED, s ? HIGH : LOW);
  digitalWrite(PIN_LED_ONBOARD, s ? LOW : HIGH);   // onboard is inverted
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== C3 alive. setup() running. ===");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_LED_ONBOARD, OUTPUT);

  // five fast blinks = "I booted"
  for (int i = 0; i < 5; i++) { ledWrite(true); delay(80); ledWrite(false); delay(80); }

  Serial.println("starting access point...");
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP("C3-TEST", "flood1234");
  Serial.print("softAP returned: "); Serial.println(ok ? "OK" : "FAILED");
  Serial.print("IP address:      "); Serial.println(WiFi.softAPIP());
  Serial.print("MAC address:     "); Serial.println(WiFi.softAPmacAddress());

  server.on("/", []() {
    server.send(200, "text/html",
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<body style='font:16px system-ui;background:#0d1418;color:#e3ecf0;padding:24px'>"
      "<h2>ESP32-C3 is alive</h2><p>WiFi and web server both work.</p>"
      "<p>Now flash the real receiver sketch.</p></body>");
  });
  server.begin();
  Serial.println("web server started. Join C3-TEST / flood1234, open http://192.168.4.1");
}

void loop() {
  server.handleClient();
  if (millis() - t > 250) {        // steady blink = loop() is running
    t = millis();
    on = !on;
    ledWrite(on);
  }
}
