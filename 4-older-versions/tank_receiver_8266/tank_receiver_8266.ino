// =====================================================================
//  TANK TEST  -  RECEIVER   (ESP8266: NodeMCU / Wemos D1 mini)
//  RA-02 / SX1278 LoRa 433 MHz  +  WiFi access point  +  web dashboard
//
//  Same job as the ESP32-C3 version, on a board whose WiFi antenna
//  actually works. Everything else - the packet, the alert logic, the
//  dashboard - is identical.
//
//  ---------------------------------------------------------------
//  TO SEE THE DASHBOARD
//    join the WiFi network   FloodNode-01
//    password                flood1234
//    open                    http://192.168.4.1
//  ---------------------------------------------------------------
//
//  ARDUINO IDE
//    Boards Manager URL: http://arduino.esp8266.com/stable/package_esp8266com_index.json
//    Board:  "NodeMCU 1.0 (ESP-12E Module)"   or  "LOLIN(WEMOS) D1 R2 & mini"
//    Upload Speed: 921600      Flash Size: 4MB (FS:2MB OTA:~1019KB)
//
//  WIRING  (silkscreen name -> what it is)
//    D5  / GPIO14 -> LoRa SCK
//    D6  / GPIO12 -> LoRa MISO
//    D7  / GPIO13 -> LoRa MOSI
//    D8  / GPIO15 -> LoRa NSS
//    D0  / GPIO16 -> LoRa RST
//    D1  / GPIO5  -> LoRa DIO0
//    D2  / GPIO4  -> 330 ohm -> LED anode, cathode to GND
//    3V3          -> LoRa VCC   (plus 470 uF at the module's pads)
//    GND          -> LoRa GND, LED cathode
//
//  The SPI pins are fixed on the ESP8266 - SCK, MISO and MOSI must be
//  D5, D6 and D7. Only NSS, RST and DIO0 are yours to choose.
// =====================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- DEMO MODE -------------------------------------------------
// Set to 1 and the receiver ignores the radio completely and simulates
// water rising from empty to over the alert line and back, every 30 s.
// Use it to prove the dashboard, the LED and the alert logic on their
// own - if the page comes alive with this set to 1, everything except
// the LoRa link is working, and you know where to look.
// Set it back to 0 for the real test.
#define DEMO_MODE  0

// ---------- access point ----------------------------------------------
const char* AP_SSID  = "FloodNode-01";
const char* AP_PASS  = "flood1234";        // 8 characters minimum
#define AP_CHANNEL  6

// ---------- pins -------------------------------------------------------
#define LORA_SS    15      // D8
#define LORA_RST   16      // D0
#define LORA_DIO0   5      // D1
#define PIN_LED     4      // D2 - external LED on PWM, active HIGH
#define HAS_ONBOARD_LED 0  // GPIO2 is now the siren relay, see below

// ---------- 220 V siren relay -----------------------------------------
// D4 / GPIO2 drives the relay module's IN pin, ACTIVE LOW.
//
// Active low is not a style choice. GPIO2 is held HIGH by the board at
// power-up and all through a reset or a flash - so an active-low module
// is de-energised whenever the ESP8266 is not deliberately driving it.
// The siren stays silent while the board boots, resets or crashes.
// Wire it active high and it screams every time you press reset.
#define PIN_SIREN        2
#define PIN_LED_ONBOARD  2      // kept so old references still compile

// ---------- the limit the siren fires at ------------------------------
// Deliberately separate from the LED's alert threshold, so the siren can
// be set higher than the indicator. GPIO2 goes LOW for this and for
// nothing else.
//
//   gap  = distance from the sensor down to the water
//   gap falls as the water rises, so "beyond the limit" means gap <= limit
#define SIREN_GAP_MM     305      // 1 ft 0 in - fire at or below this gap
#define SIREN_CLEAR_MM   380      // 1 ft 3 in - release only above this
#define SIREN_CONFIRM    3        // consecutive packets required before firing
#define SIREN_MIN_MS     30000UL  // once it sounds, it sounds for 30 s minimum

uint8_t dangerCount = 0;          // consecutive packets past the limit

bool          sirenOn = false;
bool          silenced = false;
unsigned long sirenSince = 0;
bool          stale = false;

// ---------- radio: MUST match the sender ------------------------------
#define LORA_FREQ  433E6
#define LORA_SF    7
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

#define ALERT_GAP_MM  305                 // 1 ft - for drawing the line only

#define ST_NO_ECHO   0x01
#define ST_SUB_BLIND 0x02
#define ST_ALERT     0x04

#define HIST 100

ESP8266WebServer server(80);

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

unsigned long ledNext = 0, wifiNext = 0, loraRetry = 0, beatNext = 0;
bool          ledState = false, loraReady = false;

uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

// ---------------------------------------------------------------------
//  LED brightness
//
//  The external LED is driven by PWM, so it glows in proportion to the
//  water level: dark when the column is empty, full brightness at the
//  alert line. Above the alert line it blinks instead, because a steady
//  bright LED and an alarm look the same from across a room.
// ---------------------------------------------------------------------
#define PWM_MAX  1023          // the ESP8266's analogWrite range

void ledPwm(uint16_t v) {
  if (v > PWM_MAX) v = PWM_MAX;
  analogWrite(PIN_LED, v);
}

void sirenWrite(bool on) {
  digitalWrite(PIN_SIREN, on ? LOW : HIGH);         // ACTIVE LOW
}

// Called once per accepted packet. The receiver decides for itself
// whether the level is past the limit rather than trusting the sender's
// flag, and it wants SIREN_CONFIRM readings in a row before it believes
// one. A single corrupt-but-valid packet must never start a 220 V siren.
void updateDanger() {
  bool past = (status & ST_SUB_BLIND)                       // too high to measure
           || (gapMM >= 0 && gapMM <= SIREN_GAP_MM);        // at or past the limit

  if (past) {
    if (dangerCount < 250) dangerCount++;
  } else if (gapMM >= SIREN_CLEAR_MM) {
    dangerCount = 0;                                        // clearly safe again
  }
  // between SIREN_GAP_MM and SIREN_CLEAR_MM the count is left alone:
  // that gap is the hysteresis band, and nothing changes inside it.
}

void ledWrite(bool on) { ledPwm(on ? PWM_MAX : 0); }

// The eye responds roughly to the square root of light output, so a
// straight level-to-duty mapping looks like it does nothing for the
// first half and then jumps. Squaring the fraction cancels that out and
// makes the ramp look even as the water rises.
uint16_t brightnessFor(int16_t level, int16_t depth) {
  if (level <= 0 || depth <= 0) return 0;
  float f = (float)level / (float)depth;            // 0.0 at empty, 1.0 at the sensor
  if (f > 1.0f) f = 1.0f;
  float alertF = (float)(depth - ALERT_GAP_MM) / (float)depth;
  if (alertF > 0.05f) f = f / alertF;               // full brightness AT the alert line
  if (f > 1.0f) f = 1.0f;
  return (uint16_t)(f * f * PWM_MAX + 0.5f);        // gamma 2.0
}

void pushHist(int16_t v) {
  hist[histHead] = v;
  histHead = (histHead + 1) % HIST;
  if (histN < HIST) histN++;
}

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
<button id="sil" onclick="fetch('/silence')" style="margin-left:10px;padding:6px 14px;border-radius:20px;
 border:1px solid #7d2a22;background:#2a1412;color:#ff8f7a;font-size:13px;font-weight:600;display:none">SILENCE SIREN</button>
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
      <tr><td>Siren</td><td><span id="siren">--</span></td></tr>
      <tr><td>Siren fires at</td><td><span id="sgap">--</span></td></tr>
    </table>
  </div>
</div>
</div><script>
function ftin(mm){var i=mm/25.4,f=Math.floor(i/12),r=Math.round(i%12);if(r==12){f++;r=0}return f+" ft "+r+" in"}
function load(){
 fetch('/data').then(function(r){return r.json()}).then(function(d){
  var p=document.getElementById('pill');
  document.getElementById('lora').textContent=d.lora?'listening':'OFFLINE - check wiring';
  document.getElementById('siren').textContent=d.siren?'SOUNDING':'off';
  document.getElementById('sgap').textContent='gap under '+(d.sirengap/304.8).toFixed(2)+' ft';
  document.getElementById('sil').style.display=d.siren?'inline-block':'none';
  if(!d.lora){p.className='pill st';p.textContent='RADIO OFFLINE'}
  else if(d.stale){p.className='pill st';p.textContent='STALE - LINK LOST'}
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

void startAP() {
  Serial.println();
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, 4);
  WiFi.setOutputPower(20.5);                  // maximum for the ESP8266
  Serial.print("softAP() returned : "); Serial.println(ok ? "OK" : "FAILED");
  Serial.print("  SSID            : "); Serial.println(AP_SSID);
  Serial.print("  password        : "); Serial.println(AP_PASS);
  Serial.print("  channel         : "); Serial.println(AP_CHANNEL);
  Serial.print("  IP address      : "); Serial.println(WiFi.softAPIP());
  Serial.print("  AP MAC          : "); Serial.println(WiFi.softAPmacAddress());
  Serial.println("  open http://192.168.4.1 once joined");
  Serial.println();
}

// Try to bring the radio up. Never blocks, so a missing module cannot
// stop the dashboard being served.
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
  }
  return loraReady;
}

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleSilence() {
  silenced = true;
  Serial.println("silence requested from the dashboard");
  server.send(200, "text/plain", "ok");
}

void handleData() {
  static char buf[1400];
  unsigned long age = millis() - lastHeard;
  bool fresh = (goodCount > 0) && (age < 5000);

  int n = snprintf(buf, sizeof(buf),
    "{\"ok\":%d,\"gap\":%d,\"level\":%d,\"depth\":%d,\"alert\":%d,\"blind\":%d,"
    "\"alertgap\":%d,\"sirengap\":%d,\"lora\":%d,\"siren\":%d,\"stale\":%d,\"rssi\":%d,\"snr\":%.1f,\"good\":%lu,\"bad\":%lu,\"age\":%lu,\"hist\":[",
    fresh ? 1 : 0, gapMM, levelMM, depthMM, alert ? 1 : 0,
    (status & ST_SUB_BLIND) ? 1 : 0, ALERT_GAP_MM, SIREN_GAP_MM, loraReady ? 1 : 0,
    sirenOn ? 1 : 0, stale ? 1 : 0, rssi, snr,
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
  delay(300);
  Serial.println();
  Serial.println("=== ESP8266 receiver starting ===");

  // FIRST: make the siren silent, before anything can go wrong below
  pinMode(PIN_SIREN, OUTPUT);
  sirenWrite(false);

  pinMode(PIN_LED, OUTPUT);
  analogWriteRange(PWM_MAX);
  analogWriteFreq(1000);                   // 1 kHz - no visible flicker
  ledPwm(0);
  // smooth fade up and down: proof the PWM path works, before anything else
  for (int v = 0; v <= PWM_MAX; v += 16) { ledPwm(v); delay(4); }
  for (int v = PWM_MAX; v >= 0; v -= 16) { ledPwm(v); delay(4); }

  startAP();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/silence", handleSilence);
  server.begin();
  Serial.println("web server started.");

#if DEMO_MODE
  Serial.println("*** DEMO MODE - radio ignored, water level simulated ***");
  loraReady = true;
#else
  SPI.begin();                             // ESP8266 SPI pins are fixed
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  startRadio();
#endif

  Serial.println("setup() complete.");
  lastHeard = millis();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // ---------- LED --------------------------------------------------
  //   no data   : a short wink every 2 s, so alive never looks like dead
  //   normal    : steady glow, brightness tracks the water level
  //   alert     : blinks at full brightness
  bool fresh = (goodCount > 0) && (now - lastHeard < 5000);
  if (!fresh) {
    if (now > beatNext)      { beatNext = now + 2000; ledPwm(PWM_MAX); ledNext = now + 30; ledState = true; }
    else if (ledState && now > ledNext) { ledState = false; ledPwm(0); }
  } else if (alert) {
    if (now >= ledNext) { ledState = !ledState; ledPwm(ledState ? PWM_MAX : 0); ledNext = now + 125; }
  } else {
    ledPwm(brightnessFor(levelMM, depthMM));
  }

  // ---------- stale link ----------------------------------------------
  // The LED alert used to clear itself when packets stopped. A siren must
  // NOT: losing the radio during a flood is exactly when you need it. So
  // the alert latches, the page says STALE, and the siren keeps sounding
  // until the water is confirmed safe or someone silences it by hand.
  stale = (goodCount > 0) && (now - lastHeard > 6000);

  // ---------- siren ----------------------------------------------------
  // GPIO2 is driven LOW here and in no other place in this sketch.
  bool pastLimit = (goodCount > 0) && (dangerCount >= SIREN_CONFIRM);

  if (pastLimit && !silenced && !sirenOn) {
    sirenOn = true; sirenSince = now;
    Serial.print("*** SIREN ON - gap ");
    Serial.print(gapMM); Serial.print(" mm is past the ");
    Serial.print(SIREN_GAP_MM); Serial.println(" mm limit ***");
  }
  if (sirenOn) {
    bool minElapsed = (now - sirenSince) > SIREN_MIN_MS;
    if (silenced || (!pastLimit && !stale && minElapsed)) {
      sirenOn = false;
      Serial.println(silenced ? "siren silenced by hand" : "siren off - level back below the limit");
    }
  }
  if (!pastLimit && !stale) silenced = false;   // re-arm once the water drops
  sirenWrite(sirenOn);

  // ---------- status line every 5 s -----------------------------------
  if (now > wifiNext) {
    wifiNext = now + 5000;
    Serial.print("[wifi] ip "); Serial.print(WiFi.softAPIP());
    Serial.print("  clients "); Serial.print(WiFi.softAPgetStationNum());
    Serial.print("  lora "); Serial.println(loraReady ? "ok" : "OFFLINE");
  }

#if !DEMO_MODE
  if (!loraReady && now > loraRetry) { loraRetry = now + 5000; startRadio(); }
  if (!loraReady) return;
#endif

#if DEMO_MODE
  // ---------- simulated water, no radio involved ----------------------
  static unsigned long demoNext = 0;
  static int16_t demoLevel = 0;
  static int8_t  demoDir = 1;
  if (now < demoNext) return;
  demoNext = now + 500;

  depthMM   = 914;                       // 3 ft column
  demoLevel += demoDir * 15;             // about 30 s from empty to full
  if (demoLevel >= 700) { demoLevel = 700; demoDir = -1; }
  if (demoLevel <= 0)   { demoLevel = 0;   demoDir =  1; }

  levelMM = demoLevel;
  gapMM   = depthMM - levelMM;
  alert   = (gapMM <= ALERT_GAP_MM);
  status  = alert ? ST_ALERT : 0;
  rssi    = -42;  snr = 9.5;
  goodCount++;
  lastHeard = now;
  pushHist(levelMM);
  updateDanger();

  Serial.print("DEMO  level "); Serial.print(levelMM / 304.8f, 2);
  Serial.print(" ft   gap "); Serial.print(gapMM / 304.8f, 2);
  Serial.println(alert ? " ft   ALERT" : " ft");
  return;
#endif

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
  if (gapMM >= 0 && levelMM >= 0) depthMM = gapMM + levelMM;

  alert     = (status & ST_ALERT) != 0;
  rssi      = LoRa.packetRssi();
  snr       = LoRa.packetSnr();
  goodCount++;
  lastHeard = now;

  pushHist(levelMM);
  updateDanger();

  Serial.print(alert ? "ALERT " : "ok    ");
  Serial.print("seq "); Serial.print(seq);
  if (gapMM < 0) Serial.println("   no echo");
  else {
    Serial.print("   gap ");  Serial.print(gapMM / 304.8f, 2);
    Serial.print(" ft   level "); Serial.print(levelMM / 304.8f, 2);
    Serial.print(" ft   rssi "); Serial.println(rssi);
  }
}
