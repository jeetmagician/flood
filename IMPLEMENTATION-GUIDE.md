# Aquaiots — how to implement it, from the boxes to the website

Written for Suranjeet. Follow it top to bottom. Nothing here needs you to be a
programmer: it is install, edit three lines, press upload.

> **Safety first.** The siren runs on **220 V mains**. Read
> `3-documents/Siren_220V_Circuit.pdf` properly before wiring anything to it. If you are not
> sure, have an electrician do that one connection. Everything else in this project is
> low-voltage and safe to build and test on a desk.

---

## 0. The big picture

```
 ESP32 sender          LoRa 433 MHz          ESP8266 receiver            Internet           Your website
 at the water   ───────────────────────►   indoors, on mains, on WiFi  ──────────►   api.php on hosting
 (solar, no WiFi)      kilometres            siren + LED + own page       HTTPS            ▲
                                                                                         │ browser
                                                                          Client / Admin, anywhere
```

- The **sender** measures the water and radios it. It has no WiFi and no internet.
- The **receiver** listens, sounds the siren if the water passes the danger mark, and uploads
  readings to your website. **Only the receiver touches the internet.**
- The **website** shows clients their own water and shows you, the admin, who is online —
  never anyone's water readings.
- If the internet dies, the siren, the LED and the receiver's own page (`192.168.4.1`) keep
  working. The siren never waits for the website.

You will do it in this order, and **do not skip ahead**: *desk test with no internet first*,
then hosting, then the real receiver, then outdoors.

---

## 1. What you need

**Boards and parts** (as used by the sketches; the exact wiring is in
`3-documents/Flood_Node_Circuit_Diagrams.pdf`, 19 pages — follow that, not memory):

| Part | Used for |
|---|---|
| ESP32 dev board (ESP32-WROOM-32) | the sender |
| JSN-SR04T waterproof ultrasonic sensor | measuring the water |
| RA-02 / SX1278 LoRa module, 433 MHz — **two**, one per side | the radio link |
| 433 MHz antennas — **never power a LoRa module without one** | range, and protects the module |
| ESP8266 board (NodeMCU or Wemos D1 mini) | the receiver |
| Relay module, **active-LOW**, plus the 220 V siren | the siren |
| LED + 220 Ω resistor | the level indicator |
| Solar panel + 3.7 V cell, HT7333-A regulator, **470 µF capacitor across the LoRa VCC/GND** | powering the sender at the water |

Three rules from the project that you must not break (each one fixes a bug that already
happened on real hardware):

1. A 3.7 V cell **cannot** go to the board's VIN pin. Power enters at the **3V3 pin through
   the HT7333-A**. The 470 µF capacitor is not optional — without it the radio's transmit
   spike resets the board.
2. The siren relay is **active-LOW on D4 (GPIO2)**. Never swap it for an active-high module,
   or the 220 V siren fires every time you plug in USB.
3. The sender and receiver radios must use exactly the same settings (they already do:
   433 MHz, SF7, 125 kHz, CR 4/5, sync word 0x34). Don't edit one side only.

**On your computer:** the Arduino IDE (free), a USB cable per board, your Mac for testing.

**For going live:** a domain name and a basic PHP hosting plan (about ₹1,500 a year; any
cPanel host with PHP 8.1 or newer), with the free SSL certificate switched on.

---

## 2. Set up the Arduino IDE (once)

1. Install the **Arduino IDE** from arduino.cc.
2. *Settings → Additional boards manager URLs*, add both:
   - `https://espressif.github.io/arduino-esp32/package_esp32_index.json` (ESP32)
   - `http://arduino.esp8266.com/stable/package_esp8266com_index.json` (ESP8266)
3. *Tools → Board → Boards Manager*: install **esp32** (by Espressif) and **esp8266**
   (by ESP8266 Community).
4. *Sketch → Include Library → Manage Libraries*: install **LoRa** by **Sandeep Mistry**. It
   must be this one — the sketches call `LoRa.begin()`, which only exists there. Everything
   else the sketches include comes with the board packages.

---

## 3. Flash the sender (ESP32) and set it up

File: `1-firmware/node_sender/node_sender.ino`. Nothing in it needs editing.

1. Open it in the Arduino IDE.
2. *Tools* — set **exactly** these, or the board panics on boot with
   `Detected size(4096k) smaller than the size in the binary image header`:
   - Board: **ESP32 Dev Module**
   - Flash Size: **4MB**
   - Partition Scheme: **Default 4MB with spiffs**
   - **Erase All Flash Before Sketch Upload: Enabled**
3. Pick the USB port and press **Upload**.
4. Open the Serial Monitor at **115200**. You should see `sender ready.` and one line per
   packet. If it says `LoRa init failed`, check the radio wiring and the antenna.
5. **Set the tank or river on the sender's own page** (this is where river vs tank is
   decided — the website never decides it):
   - On your phone join WiFi **`FloodNode-Setup`**, password **`12345678`**.
   - Open **`http://192.168.4.1`**.
   - Choose the profile: **RIVER** or **HOME TANK**, fill in the numbers (all in millimetres
     of water measured up from the bed or tank floor) — the depth, and the danger level
     (river) or overflow height (tank) — and press **Save**. It is stored in flash and
     survives power cuts, and it travels inside every packet, so the receiver and the website
     reconfigure themselves.

---

## 4. Desk test with no internet (do not skip)

File: `1-firmware/node_receiver_8266_offline/node_receiver_8266.ino` — the receiver *without*
the uploader, so the only thing you can get wrong is the radio.

1. Board: **NodeMCU 1.0 (ESP-12E)** or **LOLIN(WEMOS) D1 mini**, matching your board. Upload.
2. Power the sender (from your computer, on the same desk).
3. On your phone join **`FloodNode-01`**, password **`flood1234`**, open **`192.168.4.1`**.
4. **Move a book under the sensor.** The number on the page should change within a second
   or two. Make the water pass the danger mark you saved: the pill should turn to alert, the
   LED should flash, and after three packets in a row the relay should click.
5. Unplug the sender. The receiver should treat the link as lost; the siren **stays on if it
   was on** (that is deliberate — a silent siren after a dead radio is the worse failure).

When this works, the hardware is good. Only then go on. Bench-test procedure details:
`3-documents/Bench_Test_Ultrasonic_LoRa.pdf`.

---

## 5. Put the website on the internet

1. Buy a **domain** and a **PHP hosting** plan. In cPanel switch on **Let's Encrypt SSL** so
   the site opens as `https://yourdomain.in`.
2. In cPanel **File Manager → `public_html`**, upload these four files from
   `2-website/`:
   - `index.html`
   - `admin.html`
   - `api.php`
   - `.htaccess` (it keeps your database file out of reach — do not skip it)
3. **Do not upload `aquaiots.sqlite`.** That file on your Mac is test data. The live site
   creates its own empty database the first time it is used. (An existing database from
   before an upgrade is upgraded automatically.)
4. Open `https://yourdomain.in`. You should see the sign-in page with the two cards.
5. **Immediately** create your administrator: on the **Administrator** card enter your name,
   email and a password of at least 10 characters. This form works **once** and then refuses
   forever, so do it before you tell anyone the site exists. There is **no password reset** —
   keep the password safe. (If you ever lose it, the only way back is deleting the row from
   the `admins` table by hand.)

Settings at the top of `api.php` you may want to change: `KEEP_DAYS` (readings kept, 90),
`ONLINE_SEC` (180 — no upload this long means OFFLINE), `SENSOR_LOST_SEC` (30 — see section
8), `MAX_TRIES` / `LOCK_SEC` (six wrong PINs lock an account for 15 minutes).
If your host does not allow SQLite, switch to MySQL by editing the five `MYSQL_` lines at the
top; the tables are created automatically either way.

---

## 6. Flash the real receiver (ESP8266, cloud version)

File: `1-firmware/node_receiver_8266_cloud/node_receiver_8266_cloud.ino`.

At the top of the sketch edit these (nothing else):

```cpp
#define CLOUD_ENABLE   1
const char* STA_SSID   = "YOUR-WIFI-NAME";        // the WiFi where the RECEIVER sits (2.4 GHz only)
const char* STA_PASS   = "YOUR-WIFI-PASSWORD";
const char* CLOUD_HOST = "yourdomain.in";         // just the domain: no https://, no slash
#define CLOUD_HTTPS    1                          // 1 if you turned SSL on
```

Upload it. The ESP8266 cannot join a 5 GHz network. Then:

1. Open the Serial Monitor (115200). Every five seconds you get a status line, for example
   `wifi 192.168.1.40  cloud 200  PAIR CODE K7M2-9QXA`.
2. On your phone join **`FloodNode-01`** (password `flood1234`) and open **`192.168.4.1`**.
   A blue bar shows the **device key** once the receiver has reached your website.

The key is **made by your website**, not by the board: the first time a receiver contacts the
site, `api.php` creates a record for it and answers with the key, and the receiver shows it.
Until it has reached the site at least once, no key exists.

**What the receiver sends** every 25 seconds (and at once when an alert starts):

```
POST https://yourdomain.in/api.php?a=ingest
X-Token: AQI<chip id>          ← identifies the board; never shown, never typed
{"level":1840,"gap":1160,"depth":3000,"danger":2400,"mode":0,"alert":0,"rssi":-92,"snr":7.5,"age":0}
```

`age` is how many seconds ago the receiver last heard the **sensor** (-1 = never). That is
what lets the website tell a dead sensor from a quiet river (section 8).

### If it does not connect

| Serial Monitor says | Meaning |
|---|---|
| `cloud 200` | working |
| `cloud -1` | the receiver has not joined your WiFi — check name/password, and it cannot see 5 GHz |
| `cloud -2` | could not open the connection — `CLOUD_HOST` wrong, or `CLOUD_HTTPS 1` without SSL |
| `cloud -11` | timeout — weak WiFi where the receiver sits |
| `cloud 404` | `api.php` is not in `public_html`, or the path is wrong |
| `cloud 401` | the host strips the custom header — see `2-website/README.txt` for the `&token=` fallback |

If the website says "Server error", switch on PHP error logging in cPanel and read the log;
`api.php` deliberately never shows database errors in the browser.

---

## 7. Hand a client their account

1. The client (or you) reads the **eight-character device key** from the receiver's page.
2. On the website, **Client** card → **Activate a device** → type the key, name the node,
   choose a **4–8 digit PIN**, repeat it → **Activate**.
3. The site shows their **client ID** once, like `AQ-4513`. **They must write it down.**
   From then on they sign in with either the key or the client ID, plus the PIN.
4. Choose **Water tank** or **River** under the sign-in tabs. This only chooses what they are
   looking at; the real type comes from the sender. A node of the other kind shows as offline
   in that view, and live in its own.
5. If they forget their PIN there is no reset link. They read you the device key off the
   receiver (proof they hold the device) and you reset it by hand in the database.

If a client phones for help, ask for their `AQ-` number. On the **Administrator** card you
can find them by it and see whether their node is online, in alert, offline or has lost its
sensor — never their water readings, by design.

---

## 8. What the site shows, and "sensor lost"

| What the client sees | Meaning |
|---|---|
| **NORMAL / RISING / DANGER** | live; RISING is 85% of the danger mark, DANGER is the alert |
| **OFFLINE** | the receiver has not uploaded for 3 minutes (its internet or power) |
| **SENSOR LOST** | the receiver is online but has not heard the sensor for 30+ seconds. The numbers shown are grey and labelled *last known*, not live |
| **WAITING FOR SENSOR** | registered, but no sensor packet has ever arrived |

SENSOR LOST is the safety net for a remote viewer: without it a dead sender would look
online and normal, with an old level. When it appears, check the sensor unit — its power,
antenna and range. The siren on the receiver does not depend on any of this.

---

## 9. Install outdoors

1. Re-read the **power rules** in section 1 and `Flood_Node_Circuit_Diagrams.pdf` (it also
   covers where and how to install it outdoors).
2. Sender at the water: sensor face looking straight down, above the highest water you expect
   but not inside its 250 mm blind zone; antenna up and clear of metal; the enclosure sealed.
3. Receiver indoors, on mains and your WiFi, with the antenna up. Distance is limited by line
   of sight and obstacles, not by the website.
4. Final check at the real place: raise the water (or fake it) past the danger mark and
   confirm, in this order: the siren sounds, the receiver's page alerts, the website shows
   DANGER for the client, and the admin board shows the node **in alert**.

---

## 10. Moving to a new place or a new WiFi

The receiver's WiFi name and password are currently written into the sketch, so a new
location means editing `STA_SSID` / `STA_PASS` and uploading again. (A WiFi-setup box on the
receiver's own page, so no re-flashing is needed, has been proposed but not built — see the
open items in `CLAUDE.md`. The website will never ask for anyone's WiFi password.)

---

## 11. Before real customers (open items)

- **Check India's radio rules.** 433 MHz power and duty-cycle limits under the WPC low-power
  rules have **not** been verified, and India's usual commercial LoRa band is 865–867 MHz. Fine
  for prototyping; settle it before any commercial deployment.
- **No SMS fallback yet.** Where broadband is patchy, a SIM800L on the receiver sending an
  alert text would still reach someone when the internet is down.
- **Real-hardware tests are still to do:** the firmware as a whole has not been flashed from
  here, and the six-wrong-PINs lockout, MySQL mode and real cPanel hosting are untested.
- **Confirm the website address** used for the globe icon (`aquaiots.com`) and the firmware
  default (`aquaiots.in`).

---

## 12. Quick reference

| Thing | Value |
|---|---|
| Sender setup WiFi | `FloodNode-Setup` / `12345678` → `http://192.168.4.1` |
| Receiver WiFi | `FloodNode-01` / `flood1234` → `http://192.168.4.1` |
| Receiver sketch for the website | `node_receiver_8266_cloud` |
| Receiver sketch for a desk test | `node_receiver_8266_offline` |
| Radio | 433 MHz, SF7, BW 125 kHz, CR 4/5, sync 0x34, 16-byte packet |
| Upload interval | every 25 s, and immediately on alert |
| OFFLINE after | 180 s without an upload |
| SENSOR LOST after | 30 s without hearing the sensor |
| Siren | confirms on 3 packets past the limit; sounds at least 30 s; latches on link loss |
| Customer care shown on the site | +91 70024 51825 · Lachit Nagar, Guwahati 781007 |
| Run the site on your Mac | `cd 2-website && php -S 127.0.0.1:8000` |

Change the default hotspot passwords (`AP_PASS` in each sketch) before giving units to
customers: anyone who can join a hotspot can open its page.
