// =====================================================================
//  FLOOD NODE  -  SENDER   (ESP32-WROOM-32)
//  JSN-SR04T ultrasonic + RA-02 / SX1278 LoRa 433 MHz
//  + its own WiFi setup page
//
//  SET IT UP
//    join the WiFi network   FloodNode-Setup
//    password                12345678
//    open                    http://192.168.4.1
//
//  The page has two profiles - RIVER and HOME TANK - each with its own
//  measurements. Tick the one you want, fill in the numbers, press Save.
//  The settings are written to flash and survive a power cut, and they
//  travel inside every packet, so the receiver reconfigures itself with
//  no second setup page to keep in step.
//
//  ARDUINO IDE:  Board "ESP32 Dev Module"
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <LoRa.h>

// ---------- setup access point ----------------------------------------
const char* AP_SSID = "FloodNode-Setup";
const char* AP_PASS = "12345678";

// ---------- pins -------------------------------------------------------
#define PIN_TRIG   17
#define PIN_ECHO   16
#define PIN_LED     2
#define LORA_SS     5
#define LORA_RST   14
#define LORA_DIO0  26

// ---------- radio: must match the receiver ----------------------------
#define LORA_FREQ  433000000L
#define LORA_SF    7
#define LORA_BW    125E3
#define LORA_CR    5
#define LORA_SYNC  0x34

#define NODE_ID          1
#define TX_INTERVAL_MS 500
#define BLIND_MM       250        // JSN-SR04T cannot see closer than this
#define MAX_RANGE_MM  4500

#define MODE_RIVER 0
#define MODE_HOME  1

#define ST_NO_ECHO   0x01
#define ST_SUB_BLIND 0x02
#define ST_ALERT     0x04

// ---------- stored settings -------------------------------------------
// Two complete profiles are kept, so switching between them never loses
// the other one's numbers. All heights are millimetres of WATER measured
// up from the bed or the tank floor.
struct Cfg {
  uint8_t  mode;
  uint16_t rDepth;      // river: sensor face down to the bed
  uint16_t rNormal;     // river: normal water height
  uint16_t rDanger;     // river: danger level
  uint16_t hDepth;      // home:  sensor face down to the tank floor
  uint16_t hOverflow;   // home:  overflow height
} cfg;

Preferences prefs;
WebServer   server(80);

uint16_t seq   = 0;
bool     alert = false;
int16_t  lastGap = -1, lastLevel = -1;
uint8_t  lastStatus = 0;

uint16_t activeDepth()  { return cfg.mode == MODE_HOME ? cfg.hDepth    : cfg.rDepth;  }
uint16_t activeDanger() { return cfg.mode == MODE_HOME ? cfg.hOverflow : cfg.rDanger; }

void loadCfg() {
  prefs.begin("flood", false);
  cfg.mode      = prefs.getUChar ("mode",      MODE_RIVER);
  cfg.rDepth    = prefs.getUShort("rDepth",    914);    // 3 ft
  cfg.rNormal   = prefs.getUShort("rNormal",   300);
  cfg.rDanger   = prefs.getUShort("rDanger",   609);    // water 2 ft up = gap 1 ft
  cfg.hDepth    = prefs.getUShort("hDepth",   1500);    // 1.5 m tank
  cfg.hOverflow = prefs.getUShort("hOverflow",1350);
  prefs.end();
}

void saveCfg() {
  prefs.begin("flood", false);
  prefs.putUChar ("mode",      cfg.mode);
  prefs.putUShort("rDepth",    cfg.rDepth);
  prefs.putUShort("rNormal",   cfg.rNormal);
  prefs.putUShort("rDanger",   cfg.rDanger);
  prefs.putUShort("hDepth",    cfg.hDepth);
  prefs.putUShort("hOverflow", cfg.hOverflow);
  prefs.end();
  Serial.println("settings saved to flash");
}

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
  return pulseIn(PIN_ECHO, HIGH, 30000UL);
}

int cmpU32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

// median of 5. returns gap in mm, or -1 no echo, -2 inside the blind zone
int16_t measureGapMM() {
  uint32_t s[5]; uint8_t n = 0;
  for (uint8_t i = 0; i < 5; i++) {
    uint32_t t = pingOnce();
    if (t > 0) s[n++] = t;
    delay(25);
  }
  if (n < 3) return -1;
  qsort(s, n, sizeof(uint32_t), cmpU32);
  float mm = (s[n / 2] * 343.0f) / 2000.0f;
  if (mm > MAX_RANGE_MM) return -1;
  if (mm < BLIND_MM)     return -2;
  return (int16_t)(mm + 0.5f);
}

// ---------- 16-byte packet, protocol 3 --------------------------------
//  0     version = 3
//  1     node id
//  2-3   sequence
//  4-5   water level, mm from the bottom   (int16, -1 unknown)
//  6     status flags
//  7     mode: 0 river, 1 home tank
//  8-9   gap, mm from the sensor down to the water
//  10-11 danger level, mm  (river danger level, or tank overflow height)
//  12-13 depth, mm  (sensor face to the bottom)
//  14-15 CRC16-CCITT over bytes 0-13
void sendPacket(int16_t gap, int16_t level, uint8_t status) {
  uint8_t p[16];
  uint16_t danger = activeDanger(), depth = activeDepth();
  p[0]  = 3;              p[1]  = NODE_ID;
  p[2]  = seq >> 8;       p[3]  = seq & 0xFF;
  p[4]  = level >> 8;     p[5]  = level & 0xFF;
  p[6]  = status;         p[7]  = cfg.mode;
  p[8]  = gap >> 8;       p[9]  = gap & 0xFF;
  p[10] = danger >> 8;    p[11] = danger & 0xFF;
  p[12] = depth >> 8;     p[13] = depth & 0xFF;
  uint16_t c = crc16(p, 14);
  p[14] = c >> 8;         p[15] = c & 0xFF;

  LoRa.beginPacket();
  LoRa.write(p, 16);
  LoRa.endPacket();
  seq++;
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flood node setup</title><style>
*{box-sizing:border-box}
body{margin:0;background:#0d1418;color:#e3ecf0;font:15px/1.6 system-ui,-apple-system,sans-serif}
.w{max-width:620px;margin:0 auto;padding:20px}
h1{font-size:19px;font-weight:600;margin:0 0 2px}
.sub{color:#8fa3ae;font-size:13px;margin:0 0 18px}
.live{background:#16222a;border:1px solid #26353f;border-radius:10px;padding:12px 14px;margin-bottom:18px;
 display:flex;gap:18px;flex-wrap:wrap}
.live div{flex:1 1 110px}
.lab{color:#8fa3ae;font-size:10.5px;letter-spacing:.1em;text-transform:uppercase}
.val{font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}
.card{background:#16222a;border:2px solid #26353f;border-radius:12px;padding:16px;margin-bottom:14px;cursor:pointer}
.card.on{border-color:#3fa7e0;background:#16262f}
.hd{display:flex;align-items:center;gap:10px;margin-bottom:4px}
.tick{width:22px;height:22px;border-radius:50%;border:2px solid #55666f;flex:none;position:relative}
.card.on .tick{border-color:#3fa7e0;background:#3fa7e0}
.card.on .tick:after{content:"";position:absolute;left:6px;top:2px;width:6px;height:11px;
 border:solid #0d1418;border-width:0 2.5px 2.5px 0;transform:rotate(45deg)}
.ttl{font-size:16px;font-weight:600}
.hint{color:#8fa3ae;font-size:12.5px;margin:0 0 12px 32px}
.f{display:flex;align-items:center;gap:10px;margin:9px 0 9px 32px}
.f label{flex:1;font-size:13.5px;color:#c4d2d9}
.f input{width:104px;padding:7px 9px;border-radius:7px;border:1px solid #33454f;background:#0f1a20;
 color:#e3ecf0;font-size:14px;text-align:right;font-variant-numeric:tabular-nums}
.f span{width:26px;color:#8fa3ae;font-size:12.5px}
button{width:100%;padding:13px;border-radius:10px;border:0;background:#3fa7e0;color:#06222f;
 font-size:15px;font-weight:600;cursor:pointer;margin-top:4px}
.ok{background:#12382a;color:#6fd39c;border:1px solid #1e5c44;border-radius:8px;padding:9px 12px;
 margin-top:12px;font-size:13.5px;display:none}
.note{color:#8fa3ae;font-size:12px;margin-top:16px;line-height:1.55}
</style></head><body><div class="w">
<h1>Flood node setup</h1>
<p class="sub">Node 1 &middot; LoRa 433 MHz &middot; settings are stored on the board</p>

<div class="live">
  <div><div class="lab">Gap now</div><div class="val" id="g">--</div></div>
  <div><div class="lab">Water level</div><div class="val" id="l">--</div></div>
  <div><div class="lab">State</div><div class="val" id="s" style="font-size:16px">--</div></div>
</div>

<form id="fm">
<div class="card" id="cR" onclick="pick(0)">
  <div class="hd"><div class="tick"></div><div class="ttl">River</div></div>
  <p class="hint">Sensor on a bridge pier, looking down at the water.</p>
  <div class="f"><label>Sensor height above the river bed</label><input name="rd" id="rd" type="number" min="100" max="4500"><span>mm</span></div>
  <div class="f"><label>Normal water height</label><input name="rn" id="rn" type="number" min="0" max="4500"><span>mm</span></div>
  <div class="f"><label>Danger level</label><input name="rg" id="rg" type="number" min="0" max="4500"><span>mm</span></div>
</div>

<div class="card" id="cH" onclick="pick(1)">
  <div class="hd"><div class="tick"></div><div class="ttl">Home tank</div></div>
  <p class="hint">Sensor in the tank lid, looking down at the stored water.</p>
  <div class="f"><label>Tank height, floor to sensor</label><input name="hd" id="hd" type="number" min="100" max="4500"><span>mm</span></div>
  <div class="f"><label>Overflow height</label><input name="ho" id="ho" type="number" min="0" max="4500"><span>mm</span></div>
</div>

<button type="button" onclick="save()">Save and apply</button>
<div class="ok" id="okmsg">Saved. The node is using these settings now.</div>
</form>

<p class="note">Every height is measured in millimetres of water up from the bottom, not down from the sensor.
A 1.5 m tank with overflow 150 mm below the lid is tank height 1500, overflow height 1350.<br><br>
These numbers travel inside each radio packet, so the receiver picks them up automatically.</p>
</div><script>
var mode=0;
function pick(m){mode=m;
 document.getElementById('cR').className=m==0?'card on':'card';
 document.getElementById('cH').className=m==1?'card on':'card';}
function load(){
 fetch('/cfg').then(function(r){return r.json()}).then(function(c){
  document.getElementById('rd').value=c.rd; document.getElementById('rn').value=c.rn;
  document.getElementById('rg').value=c.rg; document.getElementById('hd').value=c.hd;
  document.getElementById('ho').value=c.ho; pick(c.mode);});
}
function live(){
 fetch('/live').then(function(r){return r.json()}).then(function(d){
  document.getElementById('g').textContent=d.gap<0?'--':(d.gap+' mm');
  document.getElementById('l').textContent=d.level<0?'--':(d.level+' mm');
  document.getElementById('s').textContent=d.gap<0?(d.blind?'too high':'no echo'):(d.alert?'ALERT':'normal');
  document.getElementById('s').style.color=d.alert?'#ff8f7a':'#6fd39c';
 }).catch(function(){});
}
function save(){
 var q='/save?mode='+mode;
 ['rd','rn','rg','hd','ho'].forEach(function(k){q+='&'+k+'='+document.getElementById(k).value});
 fetch(q).then(function(){var o=document.getElementById('okmsg');o.style.display='block';
  setTimeout(function(){o.style.display='none'},2500);});
}
load();live();setInterval(live,700);
</script></body></html>)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleCfg() {
  char b[200];
  snprintf(b, sizeof(b),
    "{\"mode\":%u,\"rd\":%u,\"rn\":%u,\"rg\":%u,\"hd\":%u,\"ho\":%u}",
    cfg.mode, cfg.rDepth, cfg.rNormal, cfg.rDanger, cfg.hDepth, cfg.hOverflow);
  server.send(200, "application/json", b);
}

void handleLive() {
  char b[160];
  snprintf(b, sizeof(b),
    "{\"gap\":%d,\"level\":%d,\"alert\":%d,\"blind\":%d,\"mode\":%u,"
    "\"depth\":%u,\"danger\":%u}",
    lastGap, lastLevel, alert ? 1 : 0, (lastStatus & ST_SUB_BLIND) ? 1 : 0,
    cfg.mode, activeDepth(), activeDanger());
  server.send(200, "application/json", b);
}

uint16_t clampArg(const char* name, uint16_t fallback) {
  if (!server.hasArg(name)) return fallback;
  long v = server.arg(name).toInt();
  if (v < 0) v = 0;
  if (v > 4500) v = 4500;
  return (uint16_t)v;
}

void handleSave() {
  if (server.hasArg("mode")) cfg.mode = server.arg("mode").toInt() ? MODE_HOME : MODE_RIVER;
  cfg.rDepth    = clampArg("rd", cfg.rDepth);
  cfg.rNormal   = clampArg("rn", cfg.rNormal);
  cfg.rDanger   = clampArg("rg", cfg.rDanger);
  cfg.hDepth    = clampArg("hd", cfg.hDepth);
  cfg.hOverflow = clampArg("ho", cfg.hOverflow);

  // a danger level above the sensor is meaningless - pull it back in
  if (cfg.rDanger   >= cfg.rDepth) cfg.rDanger   = cfg.rDepth   > 50 ? cfg.rDepth   - 50 : 0;
  if (cfg.hOverflow >= cfg.hDepth) cfg.hOverflow = cfg.hDepth   > 50 ? cfg.hDepth   - 50 : 0;

  saveCfg();
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED,  OUTPUT);
  digitalWrite(PIN_LED, LOW);

  loadCfg();
  Serial.print("mode: ");
  Serial.println(cfg.mode == MODE_HOME ? "HOME TANK" : "RIVER");
  Serial.print("depth "); Serial.print(activeDepth());
  Serial.print(" mm, danger "); Serial.print(activeDanger()); Serial.println(" mm");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 4);
  Serial.print("setup page: http://"); Serial.println(WiFi.softAPIP());
  Serial.print("join \""); Serial.print(AP_SSID);
  Serial.print("\" with password "); Serial.println(AP_PASS);

  server.on("/",     handleRoot);
  server.on("/cfg",  handleCfg);
  server.on("/live", handleLive);
  server.on("/save", handleSave);
  server.begin();

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
  Serial.println("sender ready.");
}

void loop() {
  server.handleClient();

  static unsigned long nextTx = 0;
  if (millis() < nextTx) return;
  nextTx = millis() + TX_INTERVAL_MS;

  uint16_t depth  = activeDepth();
  uint16_t danger = activeDanger();

  int16_t gap = measureGapMM();
  uint8_t status = 0;
  int16_t level;

  if (gap == -1) {
    status |= ST_NO_ECHO;  gap = -1;  level = -1;
  } else if (gap == -2) {
    // inside the blind zone: the water is above anything we can measure
    status |= ST_SUB_BLIND;
    gap   = BLIND_MM;
    level = depth > BLIND_MM ? depth - BLIND_MM : depth;
  } else {
    if (gap > (int16_t)depth) gap = depth;
    level = depth - gap;
  }

  // ---------- alert, with hysteresis ----------------------------------
  if (status & ST_SUB_BLIND)            alert = true;
  else if (!(status & ST_NO_ECHO)) {
    uint16_t clear = danger > 50 ? danger - 50 : 0;
    if (!alert && level >= (int16_t)danger)     alert = true;
    else if (alert && level <  (int16_t)clear)  alert = false;
  }
  if (alert) status |= ST_ALERT;

  lastGap = gap; lastLevel = level; lastStatus = status;
  sendPacket(gap, level, status);
  digitalWrite(PIN_LED, alert ? HIGH : LOW);

  Serial.print(alert ? "ALERT " : "ok    ");
  Serial.print(cfg.mode == MODE_HOME ? "[home] " : "[river] ");
  Serial.print("seq "); Serial.print(seq);
  if (gap < 0) Serial.println("   no echo");
  else {
    Serial.print("   gap ");   Serial.print(gap);
    Serial.print(" mm   level "); Serial.print(level);
    Serial.print(" / "); Serial.print(depth);
    Serial.print(" mm   danger "); Serial.print(danger);
    Serial.println(" mm");
  }
}
