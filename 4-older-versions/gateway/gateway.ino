// =====================================================================
//  FLOOD GATEWAY  —  RECEIVER
//  ESP32-WROOM-32 + SX1276 + micro-SD.  Never sleeps.
//  Listens continuously, validates, de-duplicates, publishes, logs.
// =====================================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <LoRa.h>
#include <SD.h>

const char* WIFI_SSID = "site-wifi";
const char* WIFI_PASS = "********";
const char* MQTT_HOST = "twin.example.in";
const int   MQTT_PORT = 1883;
const char* GW_ID     = "GW-KAMRUP-01";

#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26
#define SD_CS      15             // shares SCK/MOSI/MISO with the radio

WiFiClient    net;
PubSubClient  mqtt(net);

uint16_t      lastSeq[255];       // per-node duplicate filter
bool          sdReady  = false;
unsigned long lastBeat = 0;

// ---------- identical to the node's function --------------------------
uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

void connectAll() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  }
  if (!mqtt.connected()) mqtt.connect(GW_ID);
}

// ---------- every payload also goes to the card, link up or down ------
void logToCard(const char *line) {
  if (!sdReady) return;
  File f = SD.open("/log.csv", FILE_APPEND);
  if (!f) return;
  f.println(line);
  f.close();
}

void setup() {
  Serial.begin(115200);
  memset(lastSeq, 0xFF, sizeof(lastSeq));

  // One SPI bus, two devices. Start the radio first, then the card.
  SPI.begin(18, 19, 23, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  while (!LoRa.begin(865100000L)) { Serial.println("LoRa init failed"); delay(2000); }
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setSyncWord(0x34);
  LoRa.receive();                       // park in continuous receive

  sdReady = SD.begin(SD_CS, SPI);
  Serial.println(sdReady ? "SD ready" : "SD missing - buffering disabled");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectAll();
}

void loop() {
  mqtt.loop();
  if (!mqtt.connected()) connectAll();

  // --- heartbeat, so a silent gateway is distinguishable from no rain -
  if (millis() - lastBeat > 60000UL) {
    lastBeat = millis();
    char t[48];
    snprintf(t, sizeof(t), "flood/%s/status", GW_ID);
    mqtt.publish(t, "{\"alive\":true}");
  }

  int sz = LoRa.parsePacket();
  if (sz != 13) return;                 // wrong length: not one of ours

  uint8_t p[13];
  for (int i = 0; i < 13; i++) p[i] = LoRa.read();

  uint16_t got = ((uint16_t)p[11] << 8) | p[12];
  if (got != crc16(p, 11)) return;      // corrupt in the air, drop silently

  uint8_t  node = p[1];
  uint16_t seq  = ((uint16_t)p[2] << 8) | p[3];
  if (seq == lastSeq[node]) return;     // duplicate
  lastSeq[node] = seq;

  int16_t  level = (int16_t)(((uint16_t)p[4] << 8) | p[5]);
  uint16_t tips  = ((uint16_t)p[6] << 8) | p[7];
  uint16_t batt  = ((uint16_t)p[8] << 8) | p[9];
  int8_t   tempC = (int8_t)p[10];

  char topic[48], payload[220];
  snprintf(topic, sizeof(topic), "flood/%s/node/%u", GW_ID, node);
  snprintf(payload, sizeof(payload),
    "{\"node\":%u,\"seq\":%u,\"level_mm\":%d,\"rain_tips\":%u,"
    "\"rain_mm\":%.1f,\"batt_mv\":%u,\"temp_c\":%d,\"rssi\":%d,\"snr\":%.1f}",
    node, seq, level, tips, tips * 0.2f, batt, tempC,
    LoRa.packetRssi(), LoRa.packetSnr());

  mqtt.publish(topic, payload);
  logToCard(payload);
  Serial.println(payload);
}
