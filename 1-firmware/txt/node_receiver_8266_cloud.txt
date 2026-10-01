// =====================================================================
//  FLOOD NODE  -  RECEIVER   (ESP8266: NodeMCU / Wemos D1 mini)
//  RA-02 / SX1278 LoRa 433 MHz + WiFi access point + live dashboard
//  + 220 V siren relay
//
//  SEE THE DASHBOARD
//    join the WiFi network   FloodNode-01
//    password                flood1234
//    open                    http://192.168.4.1
//
//  The sender decides whether it is watching a RIVER or a HOME TANK and
//  sends its own depth and danger level inside every packet, so this
//  board has nothing to configure. The page redraws itself to match.
//
//  WIRING
//    D5/GPIO14 SCK   D6/GPIO12 MISO   D7/GPIO13 MOSI
//    D8/GPIO15 NSS   D0/GPIO16 RST    D1/GPIO5  DIO0
//    D2/GPIO4  -> 220 ohm -> LED (PWM, tracks the level)
//    D4/GPIO2  -> relay module IN  (ACTIVE LOW, drives the 220 V siren)
// =====================================================================

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- set to 1 to simulate water with no radio ------------------
#define DEMO_MODE  0

/* ===================================================================
   AQUAIOTS CLOUD UPLOAD
   Fill in your home/office WiFi and your domain, then this receiver
   keeps its own 192.168.4.1 page AND pushes every reading to the site.
   Set CLOUD_ENABLE to 0 if you want the old offline-only behaviour.
   =================================================================== */
#define CLOUD_ENABLE   1
const char* STA_SSID   = "YOUR-WIFI-NAME";
const char* STA_PASS   = "YOUR-WIFI-PASSWORD";

const char* CLOUD_HOST = "aquaiots.in";        // just the domain, no https://
const char* CLOUD_PATH = "/api.php?a=ingest";
#define CLOUD_HTTPS    1                        // 1 = https (port 443), 0 = http (80)
#define UPLOAD_SEC     25                       // seconds between normal uploads
#define UPLOAD_ALERT   1                        // also upload the moment an alert starts

const char* AP_SSID  = "FloodNode-01";
const char* AP_PASS  = "flood1234";
#define AP_CHANNEL  6

#define LORA_SS    15
#define LORA_RST   16
#define LORA_DIO0   5
#define PIN_LED     4          // D2, PWM
#define PIN_SIREN   2          // D4, ACTIVE LOW relay

#define LORA_FREQ  433E6
#define LORA_SF    7
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

#define SIREN_CONFIRM  3          // consecutive packets past the limit
#define SIREN_MIN_MS   30000UL    // minimum sounding time
#define PWM_MAX        1023

#define MODE_RIVER 0
#define MODE_HOME  1
#define ST_NO_ECHO   0x01
#define ST_SUB_BLIND 0x02
#define ST_ALERT     0x04

#define HIST 100

ESP8266WebServer server(80);

int16_t  gapMM = -1, levelMM = -1;
uint16_t depthMM = 914, dangerMM = 609;
uint8_t  mode = MODE_RIVER, status = 0;
bool     alert = false, stale = false;
int      rssi = 0;
float    snr = 0;
uint32_t goodCount = 0, badCount = 0;
uint16_t lastSeq = 0xFFFF;
unsigned long lastHeard = 0;

int16_t  hist[HIST];
uint8_t  histN = 0, histHead = 0;

bool          sirenOn = false, silenced = false, ledState = false, loraReady = false;
uint8_t       dangerCount = 0;
unsigned long sirenSince = 0, ledNext = 0, beatNext = 0, wifiNext = 0, loraRetry = 0;

// ---- cloud state -----------------------------------------------------
char          devToken[24] = "";   // derived from this chip, never changes
char          pairCode[12] = "";   // what the server tells us to show the owner
bool          cloudPaired  = false;
int           cloudLast    = 0;    // last HTTP status, 0 = never tried
unsigned long uploadNext   = 0;
bool          lastAlertSent = false;

uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

void ledPwm(uint16_t v) { analogWrite(PIN_LED, v > PWM_MAX ? PWM_MAX : v); }
void sirenWrite(bool on) { digitalWrite(PIN_SIREN, on ? LOW : HIGH); }   // ACTIVE LOW

// brightness follows the level, gamma corrected so the ramp looks even
uint16_t brightnessFor() {
  if (levelMM <= 0 || dangerMM == 0) return 0;
  float f = (float)levelMM / (float)dangerMM;
  if (f > 1.0f) f = 1.0f;
  return (uint16_t)(f * f * PWM_MAX + 0.5f);
}

void pushHist(int16_t v) {
  hist[histHead] = v;
  histHead = (histHead + 1) % HIST;
  if (histN < HIST) histN++;
}

// The receiver decides for itself whether the level is past the limit,
// and wants SIREN_CONFIRM readings in a row before it believes one.
void updateDanger() {
  bool past = (status & ST_SUB_BLIND) || (levelMM >= 0 && levelMM >= (int16_t)dangerMM);
  uint16_t clear = dangerMM > 50 ? dangerMM - 50 : 0;
  if (past) { if (dangerCount < 250) dangerCount++; }
  else if (levelMM >= 0 && levelMM < (int16_t)clear) dangerCount = 0;
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flood node</title><style>
*{box-sizing:border-box}
body{margin:0;background:#080d11;color:#e6eef3;font:15px/1.55 system-ui,-apple-system,sans-serif}
.w{max-width:900px;margin:0 auto;padding:16px}
.top{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:14px}
h1{font-size:19px;font-weight:600;margin:0}
.badge{font-size:11px;letter-spacing:.12em;text-transform:uppercase;padding:4px 10px;border-radius:6px;
 background:#12303d;color:#6fc7ef;border:1px solid #1d4a5e;font-weight:600}
.sp{flex:1}
.pill{padding:7px 16px;border-radius:22px;font-weight:700;font-size:13px;letter-spacing:.07em}
.ok{background:#0f3325;color:#5fd39a;border:1px solid #1d5f43}
.wn{background:#3a2d0e;color:#e8b64c;border:1px solid #6d5418}
.al{background:#43120f;color:#ff8f7a;border:1px solid #7d2a22;animation:fl 1s infinite}
.st{background:#22303a;color:#9db3c0;border:1px solid #33454f}
@keyframes fl{50%{opacity:.45}}
button{padding:7px 15px;border-radius:22px;border:1px solid #7d2a22;background:#2a1412;color:#ff8f7a;
 font-size:12.5px;font-weight:700;cursor:pointer;display:none;letter-spacing:.06em}
.cloud{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-bottom:14px;
 background:#0c2230;border:1px solid #1d4a5e;border-radius:12px;padding:12px 16px}
.cloud .lab{margin-bottom:2px}
.cloud #ctxt{font-size:13.5px;color:#cfe1ec}
.cloud .code{margin-left:auto;font:700 24px/1 ui-monospace,Menlo,monospace;letter-spacing:.2em;color:#6fc7ef}
.grid{display:flex;gap:14px;flex-wrap:wrap}
.card{background:#101a21;border:1px solid #1f2d36;border-radius:14px;padding:16px}
.c1{flex:1 1 230px}.c2{flex:0 0 220px}.c3{flex:1 1 300px}
.lab{color:#7e95a3;font-size:10.5px;letter-spacing:.13em;text-transform:uppercase;margin-bottom:5px}
.big{font-size:44px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums;letter-spacing:-1px}
.unit{font-size:15px;color:#7e95a3;font-weight:400;letter-spacing:0}
.sm{color:#7e95a3;font-size:12.5px;font-variant-numeric:tabular-nums;margin-top:3px}
.gap{height:16px}
.bar{height:9px;border-radius:5px;background:#1b2831;overflow:hidden;margin-top:9px}
.bar i{display:block;height:100%;width:0;border-radius:5px;background:#3fa7e0;transition:width .45s,background .3s}
svg{display:block;width:100%;height:auto}
.wv{animation:sw 3.4s linear infinite}
@keyframes sw{to{transform:translateX(-200px)}}
table{width:100%;border-collapse:collapse;font-size:12.5px;margin-top:10px}
td{padding:4px 0;color:#7e95a3;border-bottom:1px solid #18242c}
td:last-child{text-align:right;color:#e6eef3;font-variant-numeric:tabular-nums}
tr:last-child td{border-bottom:0}
</style></head><body><div class="w">

<div class="top">
  <h1>Flood node</h1><span class="badge" id="mode">--</span>
  <span class="sp"></span>
  <span id="pill" class="pill st">WAITING</span>
  <button id="sil" onclick="fetch('/silence')">SILENCE SIREN</button>
</div>

<div class="cloud" id="cloud" style="display:none">
  <div>
    <div class="lab" id="clab">Aquaiots</div>
    <div id="ctxt">--</div>
  </div>
  <div class="code" id="ccode"></div>
</div>

<div class="grid">
  <div class="card c1">
    <div class="lab" id="l1">Water level</div>
    <div class="big"><span id="v1">--</span><span class="unit" id="u1"> m</span></div>
    <div class="sm" id="s1">&nbsp;</div>
    <div class="bar"><i id="b1"></i></div>
    <div class="gap"></div>
    <div class="lab" id="l2">Below danger by</div>
    <div class="big"><span id="v2">--</span><span class="unit"> m</span></div>
    <div class="sm" id="s2">&nbsp;</div>
  </div>

  <div class="card c2">
    <div class="lab" id="vt">Column</div>
    <svg viewBox="0 0 258 330" id="viz">
      <defs>
        <clipPath id="cw"><rect id="cwr" x="44" y="24" width="112" height="276"/></clipPath>
      </defs>
      <g id="gRiver">
        <rect x="8" y="16" width="184" height="16" rx="2" fill="#243642"/>
        <rect x="78" y="40" width="44" height="9" rx="2" fill="#3a4e5c"/>
      </g>
      <g id="gTank">
        <rect x="40" y="26" width="120" height="270" rx="12" fill="#0d161c" stroke="#2b3d49" stroke-width="2"/>
        <rect x="34" y="18" width="132" height="12" rx="5" fill="#2b3d49"/>
        <rect x="82" y="30" width="36" height="8" rx="2" fill="#3a4e5c"/>
      </g>
      <g clip-path="url(#cw)">
        <rect id="wrect" x="0" y="300" width="200" height="0" fill="#1f6fa8" opacity="0.92"/>
        <g class="wv" id="wg">
          <path id="w1" d="" fill="#2f8fd0" opacity="0.95"/>
          <path id="w2" d="" fill="#2f8fd0" opacity="0.95"/>
        </g>
      </g>
      <g id="gPiers">
        <rect x="52" y="32" width="26" height="268" fill="#1a2832"/>
        <rect x="122" y="32" width="26" height="268" fill="#1a2832"/>
      </g>
      <line id="dl" x1="20" y1="120" x2="192" y2="120" stroke="#e0503c" stroke-width="2" stroke-dasharray="7 5"/>
      <text id="dt" x="198" y="116" font-size="10" fill="#e0503c">danger</text>
      <line x1="40" y1="300" x2="160" y2="300" stroke="#44586a" stroke-width="2"/>
      <text id="pc" x="100" y="176" text-anchor="middle" font-size="30" font-weight="700" fill="#e6eef3">--</text>
      <text x="100" y="322" text-anchor="middle" font-size="10" fill="#7e95a3" id="bt">bed</text>
    </svg>
  </div>

  <div class="card c3">
    <div class="lab">Level, last 50 seconds</div>
    <svg viewBox="0 0 460 150">
      <line x1="42" y1="128" x2="452" y2="128" stroke="#25333d"/>
      <line x1="42" y1="12" x2="42" y2="128" stroke="#25333d"/>
      <line id="cd" x1="42" y1="40" x2="452" y2="40" stroke="#e0503c" stroke-width="1" stroke-dasharray="6 4"/>
      <polyline id="tr" fill="none" stroke="#3fa7e0" stroke-width="2.5" stroke-linejoin="round" points=""/>
      <text x="38" y="16" font-size="10" fill="#7e95a3" text-anchor="end" id="ym">--</text>
      <text x="38" y="131" font-size="10" fill="#7e95a3" text-anchor="end">0</text>
    </svg>
    <table>
      <tr><td>Signal</td><td><span id="rs">--</span> dBm / <span id="sn">--</span> dB</td></tr>
      <tr><td>Packets good / bad</td><td><span id="gd">0</span> / <span id="bd">0</span></td></tr>
      <tr><td>Last packet</td><td><span id="ag">--</span></td></tr>
      <tr><td>Full height</td><td><span id="dp">--</span></td></tr>
      <tr><td>Danger level</td><td><span id="dg">--</span></td></tr>
      <tr><td>Radio</td><td><span id="lr">--</span></td></tr>
      <tr><td>Siren</td><td><span id="sr">--</span></td></tr>
    </table>
  </div>
</div>
</div><script>
function E(i){return document.getElementById(i)}
function m2(v){return (v/1000).toFixed(2)}
function wave(y){
 var d='M0 '+y+' q25 -7 50 0 t50 0 t50 0 t50 0 L200 330 L0 330 Z';
 E('w1').setAttribute('d',d);
 E('w2').setAttribute('d',d);
 E('w2').setAttribute('transform','translate(200,0)');
}
function load(){
 fetch('/data').then(function(r){return r.json()}).then(function(d){
  if(d.code!==undefined){
   var c=E('cloud');c.style.display='';
   if(d.wifi!=1){ E('ctxt').textContent='Joining "'+d.ssid+'" - readings are not reaching the website yet.';
                  E('ccode').textContent=''; c.style.borderColor='#6d5418'; }
   else if(d.cpaired){ E('ctxt').textContent='Signed in to aquaiots - this node is paired and uploading.';
                  E('ccode').textContent=''; c.style.borderColor='#1d5f43'; }
   else if(d.code){ E('ctxt').textContent='Type this code on the Aquaiots website to claim this node.';
                  E('ccode').textContent=d.code; c.style.borderColor='#1d4a5e'; }
   else { E('ctxt').textContent='Connected to WiFi, waiting for the first upload (status '+d.chttp+').';
                  E('ccode').textContent=''; }
  }
  var home=d.mode==1;
  E('mode').textContent=home?'Home tank':'River';
  E('gTank').style.display=home?'':'none';
  E('gRiver').style.display=home?'none':'';
  E('gPiers').style.display=home?'none':'';
  E('vt').textContent=home?'Tank':'Channel';
  E('cwr').setAttribute('x',home?44:8);
  E('cwr').setAttribute('width',home?112:184);
  E('bt').textContent=home?'floor':'bed';
  E('l1').textContent=home?'Water stored':'Water level';
  E('l2').textContent=home?'Space left':'Below danger by';

  var p=E('pill'),warn=false;
  if(!d.lora){p.className='pill st';p.textContent='RADIO OFFLINE'}
  else if(d.stale){p.className='pill st';p.textContent='LINK LOST'}
  else if(!d.ok){p.className='pill st';p.textContent='NO DATA'}
  else if(d.alert){p.className='pill al';p.textContent=home?'OVERFLOWING':'DANGER'}
  else if(d.level>=d.danger*0.8){p.className='pill wn';p.textContent='RISING';warn=true}
  else{p.className='pill ok';p.textContent=home?'NORMAL':'SAFE'}

  E('sr').textContent=d.siren?'SOUNDING':'off';
  E('sil').style.display=d.siren?'inline-block':'none';
  E('lr').textContent=d.lora?'listening':'OFFLINE - check wiring';
  E('dp').textContent=m2(d.depth)+' m';
  E('dg').textContent=m2(d.danger)+' m';
  E('rs').textContent=d.rssi; E('sn').textContent=d.snr;
  E('gd').textContent=d.good; E('bd').textContent=d.bad;
  E('ag').textContent=d.age<2000?(d.age+' ms ago'):((d.age/1000).toFixed(1)+' s ago');

  var col=d.alert?'#d8503c':(warn?'#d9a03a':'#2f8fd0');
  E('wrect').setAttribute('fill',col);
  E('w1').setAttribute('fill',col); E('w2').setAttribute('fill',col);
  E('tr').setAttribute('stroke',col); E('b1').style.background=col;

  if(d.ok&&d.level>=0){
   E('v1').textContent=m2(d.level);
   E('s1').textContent=d.level+' mm of '+d.depth+' mm';
   var pct=Math.max(0,Math.min(100,100*d.level/d.depth));
   E('b1').style.width=pct+'%';
   E('pc').textContent=Math.round(pct)+'%';
   var rem=home?(d.depth-d.level):(d.danger-d.level);
   E('v2').textContent=m2(Math.max(0,rem));
   E('s2').textContent=rem<=0?(home?'overflowing':'past the danger level'):(Math.round(rem)+' mm to go');
   var wy=300-274*d.level/d.depth;
   E('wrect').setAttribute('y',wy.toFixed(1));
   E('wrect').setAttribute('height',(300-wy).toFixed(1));
   wave(wy);
   E('wg').style.display='';
  }else{
   E('v1').textContent='--'; E('v2').textContent='--';
   E('s1').textContent=d.blind?'water above the sensor range':'no echo from the sensor';
   E('s2').textContent=' '; E('pc').textContent='--';
   E('b1').style.width='0%'; E('wg').style.display='none';
   E('wrect').setAttribute('height','0');
  }

  var dy=300-274*d.danger/d.depth;
  E('dl').setAttribute('y1',dy); E('dl').setAttribute('y2',dy);
  E('dt').setAttribute('y',dy-4);
  E('dt').textContent=home?'overflow':'danger';
  var cdy=128-116*d.danger/d.depth;
  E('cd').setAttribute('y1',cdy); E('cd').setAttribute('y2',cdy);
  E('ym').textContent=m2(d.depth);

  var pts='',n=d.hist.length;
  for(var i=0;i<n;i++){
   if(d.hist[i]<0)continue;
   pts+=(42+410*(n<2?1:i/(n-1))).toFixed(1)+','+(128-116*d.hist[i]/d.depth).toFixed(1)+' ';
  }
  E('tr').setAttribute('points',pts);
 }).catch(function(){});
}
setInterval(load,500);load();
</script></body></html>)HTML";

/* ===================================================================
   CLOUD UPLOADER
   =================================================================== */

// A token this board and only this board will ever send. It is built from
// the chip ID, so you never have to type anything into the sketch, and it
// is what the server uses to recognise the node again after a reboot.
void makeToken() {
  snprintf(devToken, sizeof(devToken), "AQI%08X%04X",
           (unsigned)ESP.getChipId(), (unsigned)(ESP.getFlashChipId() & 0xFFFF));
}

void startSTA() {
#if CLOUD_ENABLE
  WiFi.mode(WIFI_AP_STA);           // keep 192.168.4.1 alive AND join the router
  WiFi.begin(STA_SSID, STA_PASS);
  WiFi.setAutoReconnect(true);
  Serial.print("joining WiFi \""); Serial.print(STA_SSID); Serial.println("\" in the background");
#endif
}

// Pulls "code":"AB-C123" and "paired":true out of the server's reply
// without dragging in a JSON library.
void readReply(const String& r) {
  int i = r.indexOf("\"code\":\"");
  if (i >= 0) {
    int a = i + 8, b = r.indexOf('"', a);
    if (b > a && b - a < (int)sizeof(pairCode)) {
      r.substring(a, b).toCharArray(pairCode, sizeof(pairCode));
    }
  }
  cloudPaired = r.indexOf("\"paired\":true") >= 0;
}

void uploadNow() {
#if CLOUD_ENABLE
  if (WiFi.status() != WL_CONNECTED) { cloudLast = -1; return; }

  char body[220];
  snprintf(body, sizeof(body),
    "{\"level\":%d,\"gap\":%d,\"depth\":%u,\"danger\":%u,"
    "\"mode\":%u,\"alert\":%d,\"rssi\":%d,\"snr\":%.1f}",
    levelMM, gapMM, depthMM, dangerMM, mode, alert ? 1 : 0, rssi, snr);

  HTTPClient http;
  bool ok;

#if CLOUD_HTTPS
  WiFiClientSecure tls;
  tls.setInsecure();                 // the ESP8266 has no clock to check a cert date
  tls.setBufferSizes(1024, 1024);    // keep the TLS buffers small enough to fit RAM
  ok = http.begin(tls, CLOUD_HOST, 443, CLOUD_PATH, true);
#else
  WiFiClient plain;
  ok = http.begin(plain, CLOUD_HOST, 80, CLOUD_PATH, false);
#endif

  if (!ok) { cloudLast = -2; Serial.println("[cloud] could not open the connection"); return; }

  http.setTimeout(6000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Token", devToken);

  cloudLast = http.POST((uint8_t*)body, strlen(body));
  if (cloudLast == 200) readReply(http.getString());

  Serial.print("[cloud] POST -> "); Serial.print(cloudLast);
  if (pairCode[0]) {
    Serial.print("   code "); Serial.print(pairCode);
    Serial.print(cloudPaired ? "  (paired)" : "  (NOT paired yet - type this code on the website)");
  }
  Serial.println();

  http.end();
  lastAlertSent = alert;
  uploadNext = millis() + (unsigned long)UPLOAD_SEC * 1000UL;
#endif
}

void cloudLoop() {
#if CLOUD_ENABLE
  unsigned long now = millis();
  // An alert never waits for the timer.
  if (UPLOAD_ALERT && alert && !lastAlertSent && WiFi.status() == WL_CONNECTED) { uploadNow(); return; }
  if (now > uploadNext) {
    if (WiFi.status() == WL_CONNECTED) uploadNow();
    else { cloudLast = -1; uploadNext = now + 5000; }   // retry the join sooner
  }
#endif
}

void startAP() {
  Serial.println();
#if CLOUD_ENABLE
  WiFi.mode(WIFI_AP_STA);      // own hotspot AND router at the same time
#else
  WiFi.mode(WIFI_AP);
#endif
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, 4);
  WiFi.setOutputPower(20.5);
  Serial.print("softAP() returned : "); Serial.println(ok ? "OK" : "FAILED");
  Serial.print("  SSID            : "); Serial.println(AP_SSID);
  Serial.print("  password        : "); Serial.println(AP_PASS);
  Serial.print("  IP address      : "); Serial.println(WiFi.softAPIP());
  Serial.println();
}

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
  static char buf[1700];
  unsigned long age = millis() - lastHeard;
  bool fresh = (goodCount > 0) && (age < 5000);

  int n = snprintf(buf, sizeof(buf),
    "{\"ok\":%d,\"mode\":%u,\"gap\":%d,\"level\":%d,\"depth\":%u,\"danger\":%u,"
    "\"alert\":%d,\"blind\":%d,\"lora\":%d,\"siren\":%d,\"stale\":%d,"
    "\"rssi\":%d,\"snr\":%.1f,\"good\":%lu,\"bad\":%lu,\"age\":%lu,"
    "\"wifi\":%d,\"chttp\":%d,\"cpaired\":%d,\"code\":\"%s\",\"ssid\":\"%s\",\"hist\":[",
    fresh ? 1 : 0, mode, gapMM, levelMM, depthMM, dangerMM,
    alert ? 1 : 0, (status & ST_SUB_BLIND) ? 1 : 0, loraReady ? 1 : 0,
    sirenOn ? 1 : 0, stale ? 1 : 0, rssi, snr,
    (unsigned long)goodCount, (unsigned long)badCount, age,
    WiFi.status() == WL_CONNECTED ? 1 : 0, cloudLast, cloudPaired ? 1 : 0,
    pairCode, STA_SSID);

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

  // FIRST: make the siren silent, before anything below can go wrong
  pinMode(PIN_SIREN, OUTPUT);
  sirenWrite(false);

  pinMode(PIN_LED, OUTPUT);
  analogWriteRange(PWM_MAX);
  analogWriteFreq(1000);
  for (int v = 0; v <= PWM_MAX; v += 16) { ledPwm(v); delay(3); }
  for (int v = PWM_MAX; v >= 0; v -= 16) { ledPwm(v); delay(3); }

  makeToken();
  Serial.print("device token    : "); Serial.println(devToken);

  startAP();
  startSTA();
  uploadNext = millis() + 8000;        // give the join a few seconds first

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/silence", handleSilence);
  server.begin();

#if DEMO_MODE
  Serial.println("*** DEMO MODE - radio ignored, water level simulated ***");
  loraReady = true;
#else
  SPI.begin();
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  startRadio();
#endif

  lastHeard = millis();
  Serial.println("setup() complete.");
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  stale = (goodCount > 0) && (now - lastHeard > 6000);
  bool fresh = (goodCount > 0) && (now - lastHeard < 5000);

  // ---------- LED ------------------------------------------------------
  if (!fresh) {
    if (now > beatNext)       { beatNext = now + 2000; ledPwm(PWM_MAX); ledNext = now + 30; ledState = true; }
    else if (ledState && now > ledNext) { ledState = false; ledPwm(0); }
  } else if (alert) {
    if (now >= ledNext) { ledState = !ledState; ledPwm(ledState ? PWM_MAX : 0); ledNext = now + 125; }
  } else {
    ledPwm(brightnessFor());
  }

  // ---------- siren: GPIO2 is driven LOW here and nowhere else ---------
  bool pastLimit = (goodCount > 0) && (dangerCount >= SIREN_CONFIRM);
  if (pastLimit && !silenced && !sirenOn) {
    sirenOn = true; sirenSince = now;
    Serial.print("*** SIREN ON - level "); Serial.print(levelMM);
    Serial.print(" mm past the "); Serial.print(dangerMM); Serial.println(" mm limit ***");
  }
  if (sirenOn) {
    bool minElapsed = (now - sirenSince) > SIREN_MIN_MS;
    if (silenced || (!pastLimit && !stale && minElapsed)) {
      sirenOn = false;
      Serial.println(silenced ? "siren silenced by hand" : "siren off - level back below the limit");
    }
  }
  if (!pastLimit && !stale) silenced = false;
  sirenWrite(sirenOn);

  if (now > wifiNext) {
    wifiNext = now + 5000;
    Serial.print("[wifi] ip "); Serial.print(WiFi.softAPIP());
    Serial.print("  clients "); Serial.print(WiFi.softAPgetStationNum());
    Serial.print("  lora "); Serial.print(loraReady ? "ok" : "OFFLINE");
#if CLOUD_ENABLE
    Serial.print("  wifi ");
    if (WiFi.status() == WL_CONNECTED) Serial.print(WiFi.localIP());
    else                               Serial.print("joining...");
    Serial.print("  cloud "); Serial.print(cloudLast);
    if (pairCode[0] && !cloudPaired) { Serial.print("  PAIR CODE "); Serial.print(pairCode); }
#endif
    Serial.println();
  }

  cloudLoop();

#if DEMO_MODE
  static unsigned long demoNext = 0;
  static int16_t demoLevel = 0; static int8_t demoDir = 1;
  if (now < demoNext) return;
  demoNext = now + 500;
  mode = MODE_HOME; depthMM = 1500; dangerMM = 1350;
  demoLevel += demoDir * 26;
  if (demoLevel >= 1430) { demoLevel = 1430; demoDir = -1; }
  if (demoLevel <= 0)    { demoLevel = 0;    demoDir =  1; }
  levelMM = demoLevel; gapMM = depthMM - levelMM;
  alert = (levelMM >= (int16_t)dangerMM);
  status = alert ? ST_ALERT : 0;
  rssi = -42; snr = 9.5; goodCount++; lastHeard = now;
  pushHist(levelMM); updateDanger();
  return;
#endif

  if (!loraReady && now > loraRetry) { loraRetry = now + 5000; startRadio(); }
  if (!loraReady) return;

  int sz = LoRa.parsePacket();
  if (sz != 16) return;

  uint8_t p[16];
  for (int i = 0; i < 16; i++) p[i] = LoRa.read();

  uint16_t got = ((uint16_t)p[14] << 8) | p[15];
  if (got != crc16(p, 14)) { badCount++; return; }
  if (p[0] != 3) return;                       // not this protocol

  uint16_t s = ((uint16_t)p[2] << 8) | p[3];
  if (s == lastSeq) return;
  lastSeq = s;

  levelMM  = (int16_t)(((uint16_t)p[4] << 8) | p[5]);
  status   = p[6];
  mode     = p[7];
  gapMM    = (int16_t)(((uint16_t)p[8] << 8) | p[9]);
  dangerMM = ((uint16_t)p[10] << 8) | p[11];
  depthMM  = ((uint16_t)p[12] << 8) | p[13];
  if (depthMM == 0) depthMM = 1;               // never divide by zero on the page

  alert     = (status & ST_ALERT) != 0;
  rssi      = LoRa.packetRssi();
  snr       = LoRa.packetSnr();
  goodCount++;
  lastHeard = now;
  pushHist(levelMM);
  updateDanger();

  Serial.print(alert ? "ALERT " : "ok    ");
  Serial.print(mode == MODE_HOME ? "[home] " : "[river] ");
  Serial.print("seq "); Serial.print(s);
  if (levelMM < 0) Serial.println("   no echo");
  else {
    Serial.print("   level "); Serial.print(levelMM);
    Serial.print(" / ");       Serial.print(depthMM);
    Serial.print(" mm   danger "); Serial.print(dangerMM);
    Serial.print(" mm   rssi ");   Serial.println(rssi);
  }
}
