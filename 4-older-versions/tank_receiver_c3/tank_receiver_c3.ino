// =====================================================================
//  TANK TEST  -  RECEIVER   (ESP32-C3)
//  RA-02 / SX1278 LoRa 433 MHz  +  WiFi access point  +  web dashboard
//
//  Does three things:
//    1. receives the sender's level packets
//    2. blinks the LED while the water is at or above the 1 ft alert line
//    3. runs its own WiFi network and serves a live page at 192.168.4.1
//
//  ---------------------------------------------------------------
//  TO SEE THE DASHBOARD
//    join the WiFi network   FloodNode-01
//    password                flood1234
//    open                    http://192.168.4.1
//  No internet is involved. The page is served from the chip, and every
//  byte of it is in this file - nothing is fetched from a CDN.
//  ---------------------------------------------------------------
//
//  ARDUINO IDE:  Board "ESP32C3 Dev Module",  USB CDC On Boot: ENABLED
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- how the dashboard gets on the network ---------------------
//   0 = the board makes its OWN WiFi network, page at 192.168.4.1
//   1 = the board JOINS your existing router; it prints the address to
//       use on the serial monitor at startup. Try this if the access
//       point never shows up in your WiFi list.
#define USE_STATION_MODE  0

const char* AP_SSID  = "FloodNode-01";
const char* AP_PASS  = "flood1234";        // 8 characters minimum

// Channel 1 is usually the most crowded. If the network is hard to find,
// try 6, then 11 - they are the other two non-overlapping channels.
#define AP_CHANNEL  6

const char* STA_SSID = "PUT-YOUR-WIFI-NAME-HERE";
const char* STA_PASS = "PUT-YOUR-WIFI-PASSWORD-HERE";

// ---------- pins: ESP32-C3 --------------------------------------------
#define LORA_SCK    4
#define LORA_MISO   5
#define LORA_MOSI   6
#define LORA_SS     7
#define LORA_RST    3
#define LORA_DIO0  10
#define PIN_LED     1                     // external LED, active HIGH
#define PIN_LED_ONBOARD 8                 // SuperMini blue LED, active LOW
#define HAS_ONBOARD_LED 1

// ---------- radio: MUST match the sender ------------------------------
#define LORA_FREQ  433000000L
#define LORA_SF    7                      // SF7, not SF9 - see the sender
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

// ---------- for drawing the alert line only ---------------------------
// The sender decides the alert and sends it as a flag, so this constant
// only positions the dashed line on the page.
#define ALERT_GAP_MM  305                 // 1 ft

#define ST_NO_ECHO   0x01
#define ST_SUB_BLIND 0x02
#define ST_ALERT     0x04

#define HIST 100                          // samples kept for the chart

WebServer server(80);

int16_t  gapMM = -1, levelMM = -1, depthMM = 914;
uint8_t  status = 0;
bool     alert = false;
int      rssi = 0;
float    snr = 0;
uint32_t goodCount = 0, badCount = 0;
uint16_t lastSeq = 0xFFFF;
unsigned long lastHeard = 0;

int16_t  hist[HIST];
uint8_t  histN = 0, histHead = 0;

unsigned long ledNext = 0;
bool          ledState = false;

bool          loraReady = false;      // radio may come up late, or never
unsigned long wifiNext  = 0;          // AP watchdog
unsigned long loraRetry = 0;
unsigned long beatNext  = 0;

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
  digitalWrite(PIN_LED, on ? HIGH : LOW);
#if HAS_ONBOARD_LED
  digitalWrite(PIN_LED_ONBOARD, on ? LOW : HIGH);
#endif
}

void pushHist(int16_t v) {
  hist[histHead] = v;
  histHead = (histHead + 1) % HIST;
  if (histN < HIST) histN++;
}

// ---------------------------------------------------------------------
//  The dashboard. Plain HTML, inline SVG, no libraries, no web fonts.
// ---------------------------------------------------------------------
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flood Node</title><style>
*{box-sizing:border-box}
body{margin:0;background:#0d1418;color:#e3ecf0;font:15px/1.5 system-ui,-apple-system,sans-serif}
.w{max-width:760px;margin:0 auto;padding:18px}
h1{font-size:18px;font-weight:600;margin:0 0 2px}
.sub{color:#8fa3ae;font-size:13px;margin:0 0 16px}
.pill{display:inline-block;padding:5px 14px;border-radius:20px;font-weight:600;font-size:13px;letter-spacing:.06em}
.ok{background:#12382a;color:#6fd39c;border:1px solid #1e5c44}
.al{background:#4a1512;color:#ff8f7a;border:1px solid #7d2a22}
.st{background:#3a2f12;color:#e0b45a;border:1px solid #6b5520}
.row{display:flex;gap:14px;flex-wrap:wrap;margin-top:16px}
.card{background:#16222a;border:1px solid #26353f;border-radius:10px;padding:14px}
.g1{flex:1 1 210px}.g2{flex:2 1 320px}
.lab{color:#8fa3ae;font-size:11px;letter-spacing:.1em;text-transform:uppercase;margin-bottom:4px}
.big{font-size:38px;font-weight:600;line-height:1.1;font-variant-numeric:tabular-nums}
.unit{font-size:15px;color:#8fa3ae;font-weight:400}
.mm{color:#8fa3ae;font-size:12px;font-variant-numeric:tabular-nums}
table{width:100%;border-collapse:collapse;font-size:12.5px;margin-top:4px}
td{padding:3px 0;color:#8fa3ae}td:last-child{text-align:right;color:#e3ecf0;font-variant-numeric:tabular-nums}
svg{display:block;width:100%;height:auto}
</style></head><body><div class="w">
<h1>Flood node &mdash; live</h1>
<p class="sub">LoRa 433 MHz &middot; served from the receiver at 192.168.4.1</p>
<span id="pill" class="pill ok">WAITING</span>
<div class="row">
  <div class="card g1">
    <div class="lab">Height remaining</div>
    <div class="big"><span id="gapft">--</span><span class="unit"> ft</span></div>
    <div class="mm" id="gapin">sensor to water</div>
    <div style="height:14px"></div>
    <div class="lab">Water level</div>
    <div class="big"><span id="lvlft">--</span><span class="unit"> ft</span></div>
    <div class="mm" id="lvlin">depth of water</div>
  </div>
  <div class="card" style="flex:0 0 150px">
    <div class="lab">Column</div>
    <svg viewBox="0 0 120 330" id="tank">
      <rect x="34" y="14" width="52" height="12" rx="2" fill="#3a4a55"/>
      <text x="60" y="10" font-size="8" fill="#8fa3ae" text-anchor="middle">sensor</text>
      <rect x="30" y="30" width="60" height="280" fill="#101b21" stroke="#2c3d47"/>
      <rect id="water" x="31" y="309" width="58" height="0" fill="#2f86c4" opacity="0.85"/>
      <line id="aline" x1="26" y1="100" x2="94" y2="100" stroke="#e05a44" stroke-width="1.5" stroke-dasharray="5 4"/>
      <text id="atext" x="96" y="98" font-size="8" fill="#e05a44">1 ft</text>
      <line x1="30" y1="310" x2="90" y2="310" stroke="#55666f" stroke-width="2"/>
      <text x="60" y="324" font-size="8" fill="#8fa3ae" text-anchor="middle">bed</text>
    </svg>
  </div>
  <div class="card g2">
    <div class="lab">Water level, last 50 seconds</div>
    <svg viewBox="0 0 480 150" id="chart">
      <line x1="40" y1="130" x2="470" y2="130" stroke="#2c3d47"/>
      <line x1="40" y1="10" x2="40" y2="130" stroke="#2c3d47"/>
      <line id="calert" x1="40" y1="40" x2="470" y2="40" stroke="#e05a44" stroke-width="1" stroke-dasharray="5 4"/>
      <polyline id="trace" fill="none" stroke="#3fa7e0" stroke-width="2" points=""/>
      <text x="36" y="14" font-size="10" fill="#8fa3ae" text-anchor="end" id="ymax">3.0</text>
      <text x="36" y="133" font-size="10" fill="#8fa3ae" text-anchor="end">0</text>
      <text x="8" y="80" font-size="10" fill="#8fa3ae" transform="rotate(-90 8 80)" text-anchor="middle">ft</text>
    </svg>
    <table>
      <tr><td>Signal</td><td><span id="rssi">--</span> dBm / <span id="snr">--</span> dB</td></tr>
      <tr><td>Packets good / bad</td><td><span id="good">0</span> / <span id="bad">0</span></td></tr>
      <tr><td>Last packet</td><td><span id="age">--</span></td></tr>
      <tr><td>Column depth</td><td><span id="depth">--</span></td></tr>
      <tr><td>Radio</td><td><span id="lora">--</span></td></tr>
    </table>
  </div>
</div>
</div><script>
function ftin(mm){var i=mm/25.4,f=Math.floor(i/12),r=Math.round(i%12);if(r==12){f++;r=0}return f+" ft "+r+" in"}
function load(){
 fetch('/data').then(function(r){return r.json()}).then(function(d){
  var p=document.getElementById('pill');
  document.getElementById('lora').textContent=d.lora?'listening':'OFFLINE - check wiring';
  if(!d.lora){p.className='pill st';p.textContent='RADIO OFFLINE'}
  else if(!d.ok){p.className='pill st';p.textContent='NO DATA'}
  else if(d.alert){p.className='pill al';p.textContent='ALERT'}
  else{p.className='pill ok';p.textContent='NORMAL'}
  var D=d.depth||914;
  if(d.ok&&d.gap>=0){
   document.getElementById('gapft').textContent=(d.gap/304.8).toFixed(2);
   document.getElementById('lvlft').textContent=(d.level/304.8).toFixed(2);
   document.getElementById('gapin').textContent=ftin(d.gap)+"  ("+d.gap+" mm)";
   document.getElementById('lvlin').textContent=ftin(d.level)+"  ("+d.level+" mm)";
   var h=280*d.level/D;
   var w=document.getElementById('water');
   w.setAttribute('height',h.toFixed(1));
   w.setAttribute('y',(310-h).toFixed(1));
   w.setAttribute('fill',d.alert?'#c9503c':'#2f86c4');
  }else{
   document.getElementById('gapft').textContent='--';
   document.getElementById('lvlft').textContent='--';
   document.getElementById('gapin').textContent=d.blind?'water inside the blind zone':'no echo';
   document.getElementById('lvlin').textContent='';
  }
  var ay=310-280*(D-d.alertgap)/D;
  document.getElementById('aline').setAttribute('y1',ay);
  document.getElementById('aline').setAttribute('y2',ay);
  document.getElementById('atext').setAttribute('y',ay-2);
  var cay=130-120*(D-d.alertgap)/D;
  document.getElementById('calert').setAttribute('y1',cay);
  document.getElementById('calert').setAttribute('y2',cay);
  document.getElementById('ymax').textContent=(D/304.8).toFixed(1);
  var pts='',n=d.hist.length;
  for(var i=0;i<n;i++){
   if(d.hist[i]<0)continue;
   var x=40+430*(n<2?1:i/(n-1));
   var y=130-120*d.hist[i]/D;
   pts+=x.toFixed(1)+','+y.toFixed(1)+' ';
  }
  document.getElementById('trace').setAttribute('points',pts);
  document.getElementById('trace').setAttribute('stroke',d.alert?'#e05a44':'#3fa7e0');
  document.getElementById('rssi').textContent=d.rssi;
  document.getElementById('snr').textContent=d.snr;
  document.getElementById('good').textContent=d.good;
  document.getElementById('bad').textContent=d.bad;
  document.getElementById('age').textContent=d.age<2000?(d.age+" ms ago"):((d.age/1000).toFixed(1)+" s ago");
  document.getElementById('depth').textContent=(D/304.8).toFixed(2)+" ft ("+D+" mm)";
 }).catch(function(){});
}
setInterval(load,400);load();
</script></body></html>)HTML";

// Try to bring the radio up. Returns immediately either way, so a missing
// or miswired module can never stop the dashboard from being served.
bool startRadio() {
  if (LoRa.begin(LORA_FREQ)) {
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setSyncWord(LORA_SYNC);
    LoRa.receive();
    loraReady = true;
    Serial.println("LoRa OK - listening on 433.0 MHz, SF7.");
  } else {
    loraReady = false;
    Serial.println("LoRa NOT responding - check SPI wiring and the antenna.");
    Serial.println("The dashboard still works; it will show 'radio offline'.");
  }
  return loraReady;
}

// Bring WiFi up, whichever mode is selected, and say exactly what happened.
void startAP() {
  Serial.println();
#if USE_STATION_MODE
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Serial.print("joining \""); Serial.print(STA_SSID); Serial.println("\" ...");
  WiFi.begin(STA_SSID, STA_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("  connected. OPEN THIS ADDRESS: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("  FAILED to join - check STA_SSID and STA_PASS.");
    Serial.println("  Note: the C3 is 2.4 GHz only. A 5 GHz-only network will never work.");
  }
#else
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);                       // no modem sleep on an AP
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, 4);  // visible, 4 clients
  WiFi.setTxPower(WIFI_POWER_19_5dBm);        // maximum - the C3 SuperMini needs it
  Serial.print("softAP() returned : "); Serial.println(ok ? "OK" : "FAILED");
  Serial.print("  channel         : "); Serial.println(AP_CHANNEL);
  Serial.print("  tx power        : "); Serial.print(WiFi.getTxPower() / 4.0, 1);
  Serial.println(" dBm");
  Serial.print("  SSID            : "); Serial.println(AP_SSID);
  Serial.print("  password        : "); Serial.println(AP_PASS);
  Serial.print("  IP address      : "); Serial.println(WiFi.softAPIP());
  Serial.print("  AP MAC          : "); Serial.println(WiFi.softAPmacAddress());
  Serial.println("  open http://192.168.4.1 once joined");
#endif
  Serial.println();
}

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
  static char buf[1400];      // static: a 1.4 kB frame is a lot of stack in a handler
  unsigned long age = millis() - lastHeard;
  bool fresh = (goodCount > 0) && (age < 5000);

  int n = snprintf(buf, sizeof(buf),
    "{\"ok\":%d,\"gap\":%d,\"level\":%d,\"depth\":%d,\"alert\":%d,\"blind\":%d,"
    "\"alertgap\":%d,\"lora\":%d,\"rssi\":%d,\"snr\":%.1f,\"good\":%lu,\"bad\":%lu,\"age\":%lu,\"hist\":[",
    fresh ? 1 : 0, gapMM, levelMM, depthMM, alert ? 1 : 0,
    (status & ST_SUB_BLIND) ? 1 : 0, ALERT_GAP_MM, loraReady ? 1 : 0, rssi, snr,
    (unsigned long)goodCount, (unsigned long)badCount, age);

  for (uint8_t i = 0; i < histN && n < (int)sizeof(buf) - 12; i++) {
    uint8_t idx = (histHead + HIST - histN + i) % HIST;
    n += snprintf(buf + n, sizeof(buf) - n, "%s%d", i ? "," : "", hist[idx]);
  }
  snprintf(buf + n, sizeof(buf) - n, "]}");
  server.send(200, "application/json", buf);
}

void setup() {
  Serial.begin(115200);
  delay(400);

  pinMode(PIN_LED, OUTPUT);
#if HAS_ONBOARD_LED
  pinMode(PIN_LED_ONBOARD, OUTPUT);
#endif
  ledWrite(false);
  // five fast blinks: proof the sketch is running, before anything can fail
  for (int i = 0; i < 5; i++) { ledWrite(true); delay(80); ledWrite(false); delay(80); }

  startAP();
  Serial.print("AP  \""); Serial.print(AP_SSID);
  Serial.print("\"  password "); Serial.println(AP_PASS);
  Serial.print("dashboard: http://"); Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  startRadio();                  // does NOT block if the radio is missing

  Serial.println("setup() complete.");
  lastHeard = millis();
}

void loop() {
  server.handleClient();

  // ---------- LED: blinks while the alert stands ----------------------
  unsigned long now = millis();
  if (alert) {
    if (now >= ledNext) { ledState = !ledState; ledWrite(ledState); ledNext = now + 125; }
  } else if (ledState && now > ledNext) {
    ledState = false; ledWrite(false);
  }

  // Heartbeat: one short wink every 2 s while nothing is arriving, so a
  // live-but-idle receiver looks different from a dead one.
  if (!alert && now - lastHeard > 3000 && now > beatNext) {
    beatNext = now + 2000;
    ledWrite(true); ledNext = now + 30; ledState = true;
  }

  // ---------- WiFi watchdog + heartbeat on the serial port ------------
  if (now > wifiNext) {
    wifiNext = now + 5000;
#if USE_STATION_MODE
    Serial.print("[wifi] station ip "); Serial.print(WiFi.localIP());
    Serial.print("  rssi "); Serial.print(WiFi.RSSI());
    Serial.print("  lora "); Serial.println(loraReady ? "ok" : "OFFLINE");
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[wifi] link lost - rejoining"); startAP(); }
#else
    IPAddress ip = WiFi.softAPIP();
    Serial.print("[wifi] ip "); Serial.print(ip);
    Serial.print("  clients "); Serial.print(WiFi.softAPgetStationNum());
    Serial.print("  lora "); Serial.println(loraReady ? "ok" : "OFFLINE");
    if (ip == IPAddress(0, 0, 0, 0)) {
      Serial.println("[wifi] access point is DOWN - restarting it");
      startAP();
    }
#endif
  }

  // Radio missing at boot? Keep trying quietly, every 5 s.
  if (!loraReady && now > loraRetry) { loraRetry = now + 5000; startRadio(); }
  if (!loraReady) return;

  // ---------- stale link: drop the alert rather than lie --------------
  if (alert && now - lastHeard > 6000) {
    alert = false;
    Serial.println("alert cleared - no packets, level unknown");
  }

  int sz = LoRa.parsePacket();
  if (sz != 13) return;

  uint8_t p[13];
  for (int i = 0; i < 13; i++) p[i] = LoRa.read();

  uint16_t got = ((uint16_t)p[11] << 8) | p[12];
  if (got != crc16(p, 11)) { badCount++; return; }

  uint16_t seq = ((uint16_t)p[2] << 8) | p[3];
  if (seq == lastSeq) return;
  lastSeq = seq;

  levelMM = (int16_t)(((uint16_t)p[4] << 8) | p[5]);
  status  = p[6];
  gapMM   = (int16_t)(((uint16_t)p[8] << 8) | p[9]);
  if (gapMM >= 0 && levelMM >= 0) depthMM = gapMM + levelMM;   // scale, from the packet itself

  alert     = (status & ST_ALERT) != 0;
  rssi      = LoRa.packetRssi();
  snr       = LoRa.packetSnr();
  goodCount++;
  lastHeard = now;

  pushHist(levelMM);

  if (!alert) { ledWrite(true); delay(20); ledWrite(false); }   // tick per packet

  Serial.print(alert ? "ALERT " : "ok    ");
  Serial.print("seq "); Serial.print(seq);
  if (gapMM < 0) Serial.println("   no echo");
  else {
    Serial.print("   gap ");  Serial.print(gapMM / 304.8f, 2);
    Serial.print(" ft   level "); Serial.print(levelMM / 304.8f, 2);
    Serial.print(" ft   rssi "); Serial.println(rssi);
  }
}
