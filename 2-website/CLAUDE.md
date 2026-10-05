# Aquaiots

LoRa water-level telemetry. An ultrasonic sensor on a river pier or a rooftop
tank reports to a receiver kilometres away, which sounds a siren locally and
pushes readings to a website the owner can open from anywhere.

Founder: Suranjeet Choudhury. Guwahati, Assam. Built for Indian conditions:
monsoon flooding, patchy internet, no mains power at the water.

---

## The chain

```
ESP32 + JSN-SR04T   --433 MHz LoRa-->   ESP8266 + relay   --HTTPS-->   api.php   <--poll--   browser
(at the water,                          (indoors, mains,              (shared              (anywhere)
 solar, no internet)                     on the WiFi)                  hosting)
```

**Only the receiver touches the internet.** The sender has no WiFi, no SIM and
no clock. This is the central design fact — do not propose moving the uplink to
the sender, and do not add a cloud dependency to anything the sender does.

Everything local keeps working with the internet down: the receiver's own
dashboard at 192.168.4.1, the indicator LED, and the siren. The siren decides on
the receiver from the last three packets and never waits for the server.

---

## Repository layout

```
1-firmware/
  node_sender/                    ESP32. Ultrasonic + LoRa TX + setup AP.
  node_receiver_8266_cloud/       ESP8266. LoRa RX + local dashboard + uploader.  <- current
  node_receiver_8266_offline/     Same without the uploader.
  txt/                            The same three sketches as plain .txt, for sharing
                                  or pasting. Copies: the .ino files are the source,
                                  so re-copy after any firmware edit.
2-website/
  index.html                      The whole site. Start screen (#login) has two doors,
                                  Client and Administrator; then the client dashboard
                                  (#client), the admin board (#admin) and the public
                                  story (#how). The server session guards the data.
  admin.html                      Redirect to index.html#admin, for old bookmarks.
  api.php                         The entire backend. Device ingest + both portals.
  .htaccess                       Denies the SQLite file to the browser.
  README.txt                      Deployment instructions.
3-documents/                      Circuit diagrams and test procedures (PDF).
4-older-versions/                 Superseded sketches. Do not edit or revive.
START-HERE.txt                    One-page tour of the folder, for the founder.
.github/workflows/deploy.yml      Auto-deploy: a push to main that changes 2-website/ uploads
                                  the four site files to Hostinger over FTPS.
IMPLEMENTATION-GUIDE.md           Step-by-step: boards, Arduino IDE, desk test, hosting,
                                  receiver, client hand-over, outdoors. Start here to build it.
```

---

## Hard constraints

These were each arrived at by debugging real hardware. Changing one re-opens a
bug that is already closed.

**The siren relay is active-LOW on GPIO2.** GPIO2 is an ESP8266 strapping pin
held HIGH through boot, reset and flashing. Active-low means HIGH is silent, so
the siren stays quiet through every reboot and every firmware upload. An
active-high module makes a 220 V siren fire every time the USB cable is plugged
in. Never invert this.

**The siren latches on link loss; the LED clears.** Opposite behaviours, on
purpose. If the radio dies while the water was high, a silent siren is the worse
failure. Do not "fix" this inconsistency.

**A 3.7 V cell cannot feed VIN.** The onboard AMS1117 needs about 4.8 V. Power
enters at the 3V3 pin through an HT7333-A. A 470 µF capacitor sits across the
LoRa module's VCC and GND; without it the 120 mA transmit spike browns out the
board.

**ESP32 board settings: Flash Size 4MB, Partition Default 4MB with spiffs,
Erase All Flash enabled.** Otherwise it panics on boot with
`Detected size(4096k) smaller than the size in the binary image header`.

**LoRa: 433 MHz, SF7, BW 125 kHz, CR 4/5, sync word 0x34.** SF7 because the
bench rate is 2 Hz — SF9 would put a 16-byte packet at 165 ms airtime, a 33%
duty cycle. Both ends must match exactly.

**The LoRa library is Sandeep Mistry's `LoRa`.** Not RadioLib, not any LoRaWAN
stack. The sketches call `LoRa.begin()`, which only exists there.

**Fixed `char` buffers, never Arduino `String`, in the firmware.** The ESP8266
has roughly 40 KB free with TLS buffers allocated. `String` fragments the heap
and crashes after hours, which is the worst kind of bug in an alarm system.

**`tls.setBufferSizes(1024, 1024)` is load-bearing.** The default 16 KB TLS
buffers do not fit. `setInsecure()` is deliberate: the ESP8266 has no clock, so
it cannot check certificate dates. Traffic is still encrypted.

**The ESP8266 runs `WIFI_AP_STA`.** Its own hotspot and the router at the same
time. Dropping to `WIFI_STA` kills the local dashboard, which is the fallback
when the internet is down.

---

## The packet

16 bytes, protocol version 3:

| Byte | Field |
|---|---|
| 0 | version, always 3 |
| 1 | node id |
| 2-3 | sequence |
| 4-5 | level, mm |
| 6 | status bits |
| 7 | mode: 0 river, 1 home tank |
| 8-9 | gap to sensor, mm |
| 10-11 | danger mark, mm |
| 12-13 | full depth, mm |
| 14-15 | CRC16-CCITT over bytes 0-13 |

Status bits: `0x01` no echo, `0x02` inside the sensor's blind zone,
`0x04` alert.

**The sender decides river or tank**, from its own setup page, and the whole
website re-labels itself from byte 7. One node is one mode at a time. The
website never sets the mode.
The client sign-in has a Water tank / River choice, but that is only a view
filter: a node of the other kind shows as offline in it. It never writes a mode.

---

## Security decisions

**The site never asks for a WiFi password and must never start.** Several
earlier drafts proposed "log in with your SSID and password" — it does not
identify anyone (SSIDs are not unique), the server cannot reach out to a device
anyway, and storing customers' home WiFi credentials is a liability no IoT
company accepts. If a request heads this way, say so rather than building it.

**A device identifies itself with a token derived from its chip ID**, sent in
the `X-Token` header. It is never displayed and never typed.

**A client signs in with a device key plus a PIN.** The key is eight characters
the receiver prints on its own page, from an alphabet with no O/0 or I/1.
Holding the key proves physical possession of the device, which is the only
thing that actually matters. A client ID (`AQ-4513`) is generated on activation
and also works as a login handle.

**Wrong key and wrong PIN return an identical message.** The login form must not
reveal which keys exist. Six failures lock the account for 15 minutes.

**The admin portal CAN see water readings (decision, founder, 2026-10-06).** It used to be
status-only, a promise that could be made to customers in writing; the founder chose to end it so
the administrator sees every client's nodes as the client does. So: do not promise customers that
Aquaiots staff cannot see their data. `admin_clients` returns each node's latest reading, taken from
the `devices.lv_*` columns (it still never reads `readings`/`history`, so the 5 s admin poll stays
cheap). The 5-minute record is read per node through `a=history` and `a=export`, which accept a
client session (own nodes only) or an admin session (any node) via `device_for_reader()`. Client PINs
are still hashed and never retrievable, and the start screen still reveals nothing about which keys exist.

**`admin_setup` works exactly once**, while the admins table is empty, then
refuses forever. There is no password reset and no second admin. On the site it
is the Administrator card of the start screen turning into a setup form on first
run, so create the admin before sharing the address.

**The Administrator sign-in is visible on the start screen, by decision.** An
earlier design hid it behind `#admin`; the founder chose two visible doors. The
form being visible grants nothing: every admin call checks the server session,
and `#admin` or `#client` without one sends the visitor back to `#login`.

**Client and admin share one PHP session cookie.** `logout` unsets only
`$_SESSION['cid']` and `admin_logout` only `['aid']`, so one sign-out never ends
the other. Do not go back to destroying the whole session.

**Never commit credentials, the database or test data.** `aquaiots.sqlite*` is in
`.gitignore` (it holds the admin hash and clients). The admin's email and password
are typed into the setup form and live only in the database: never in a file, never
in a doc. The repo is public.

**Database errors go to the PHP error log, never to the browser** — they leak
table names and paths. Every response path is `fail('Server error.', 500)`.

---

## The website

One file, `index.html`, four views switched by the address hash and `route()`:

| Hash | View | Who |
|---|---|---|
| none or `#login` | Start screen: Client card and Administrator card, side by side | everyone |
| `#how` `#features` `#founder` `#contact` `#about` | The public story and Contact | everyone |
| `#client` | Client dashboard | needs a client session |
| `#admin` | Administrator status board | needs an admin session |

A browser that already holds a session skips the start screen on first load.
`admin.html` is only a redirect to `index.html#admin` so old bookmarks work.

**Water tank / River on the client card.** A segmented choice on the sign-in and
activate tabs, and a switch in the dashboard header, remembered in `localStorage`
(`aq_kind`). It is a view filter and nothing more: a node whose byte-7 mode does
not match the chosen view is drawn as OFFLINE with no numbers and a note telling
the client which view shows it. The server never receives the choice and no mode
is ever written. Do not turn it into a setting.

**Node states on the client dashboard:** NORMAL, RISING, DANGER/OVERFLOWING, OFFLINE
(no upload for 180 s), **SENSOR LOST** (receiver online, sensor unheard: grey water,
values labelled "last known", an amber note) and **WAITING FOR SENSOR** (registered,
no sensor packet ever; belongs to whichever Tank/River view is open, since its kind is
not known yet). On the admin board these are the pills `ok / alert / sensor lost /
offline / waiting`, a "Sensor lost" count and filter. They come from `devices.sensor_ok`
and the last alert flag only, so the admin board still never touches `level_mm`.

**Live data and reports (client dashboard).** The dashboard polls `a=nodes` every 2 s
(skipping a poll if the last one is still in flight) and shows `LIVE - updated N s ago`.
The level chart has a **Live | 7 days** switch (`a=history&range=live|7d`; live
refreshes every 4 s, 7 days every minute). A **Download a report (PDF)** card lets the
client pick Day, Month or Year (Day uses a date input; Month a month select plus year;
no `type=month`, which Safari lacks) and calls `a=export&device&from&to`. If nothing
was recorded the page says so and downloads nothing. The PDF is built in the browser by
`AqPdf` (plain ES5 inside `index.html`: A4, built-in Helvetica/Courier, a JPEG logo drawn
from the header image on a canvas, a hand-written xref table), so no library, no build
step and no server load. A report holds the site (node) name, client ID, device key,
type, full depth, danger/overflow mark, period, generated time with time zone, summary
boxes (readings, highest, lowest, average, time at/over the mark, alerts), a chart with
the danger line, and a paginated table (date and time, level, % of depth, status,
signal), preceded by an alert-events section. Every PDF has a header with the logo, the founder's
name, customer care number and address. Times are the viewer's local time and say so.

**Records card (both portals, `Recs()` in `index.html`).** The 5-minute log for the last 7 days: a day
selector (All 7 days, or one day), All readings | Alerts only, date / time / level / % depth / status /
signal, the alert events (a run of alert readings is one event) and a Download PDF button. The client
gets it under the chart; the administrator gets it by clicking a node on the board, which opens a detail
panel (figures, Live | 7 days chart, Records, PDF). The board polls every 5 s. The admin's PDF is
`AqPdf.adminReport`: totals plus one row per node with its latest level.

**Wrong key, wrong PIN and unknown key still give one message.** Nothing on the
start screen may reveal which keys or client IDs exist.

**Brand and contact** (all public, all in `index.html`):
- Logo: the full drop-and-wordmark logo in the top bar and on the start screen,
  drop-only as favicon. Background removed so it sits on the dark panels; do not
  recolour it. The dark-blue "Aqua" is faint on dark by design of the logo.
- Founder photo: embedded, square crop, in the Founder section.
- Customer care: +91 70024 51825. Address: Lachit Nagar, Guwahati, Assam 781007.
- Facebook: https://www.facebook.com/share/1DrZnPoUhd/
- Instagram: https://www.instagram.com/magician_jeet?stkn=MW9nNmh0Nmo3ODNwOQ==
- The globe icon links to `https://aquaiots.com/`, which the founder has not
  confirmed (the firmware defaults to `aquaiots.in`). Ask before changing.
- Contact appears as a section on the story page and in the footer of every view.

---

## Conventions

- `api.php` is deliberately one file with no dependencies, no composer, no
  framework. It must keep running on ₹1,500/year cPanel shared hosting with
  nothing but PHP 8.1. Do not introduce a build step, a package manager or a
  framework.
- SQLite by default; MySQL is a five-constant switch at the top. The schema is
  created by the file itself on first request.
- The front end is plain ES5-era JavaScript in one `<script>`, no framework, no
  bundler, no npm. Pages are self-contained single files: the logo, founder photo
  social icons and the circuit texture are embedded as base64 data URIs
  (WebP/PNG), so the upload is still just the four site files. Keep it that way;
  index.html is about 320 KB because of them.
- Design tokens live in `:root` in each page. Dark is the default and the brand
  look; a light theme exists behind a switch in the top bar (the founder asked for
  it). Light overrides live in `:root[data-theme="light"]`, the choice is kept in
  `localStorage` as `aq_theme`, and a tiny script in `<head>` applies it before
  first paint. Never hard-code a colour in CSS or SVG: use a token (`var(--line)`,
  `var(--tank-in)`, `--ok-bg` ...) so both themes work. SVG fills that need a
  token go in `style="fill:var(--x)"`, because attributes cannot hold `var()`.
  Display face Bricolage Grotesque, body IBM Plex Sans, mono IBM Plex Mono.
  Accent `--cy: #37C6E8` (darkened to `#0B7FA6` in light for text contrast).
  Status colours are separate from the accent.
- Background: a circuit-board photo (IoT flavour) sits behind every view as a
  faint texture. It is stored as a luminance-to-alpha WebP in `--pcb` and used as a
  CSS mask on `body::before`, coloured by `--pcb-color` at `--pcb-op` opacity, so
  one image serves both themes and never competes with text. Keep the opacity low.
  The source photo is only 480x360, so never show it as a plain full-bleed picture.
- All distances are millimetres, integer, everywhere — firmware, wire format,
  database, API. Metres appear only in rendered text.
- Comments in the firmware explain *why*, especially where a line looks wrong
  but is load-bearing. Keep them when editing.

---

## Working on this

The firmware cannot be compiled or flashed by an agent — it needs the Arduino
IDE and a physical board. Changes to `1-firmware/` are suggestions for a human
to flash and test.

`index.html` cannot be previewed meaningfully without PHP behind it; opened as a
plain file or from a static server, every sign-in fails. That is expected, not a
bug. PHP is installed on the founder's Mac (Homebrew, PHP 8.5): run
`php -S 127.0.0.1:8000` in `2-website/` and test against that. A simulated
receiver is one POST (use `"mode":1` for a tank; wait about 1 s between posts, the
server throttles faster ones):

```
curl -X POST 'http://127.0.0.1:8000/api.php?a=ingest' \
  -H 'X-Token: AQIDEMO000000001' -H 'Content-Type: application/json' \
  -d '{"level":1840,"gap":1160,"depth":3000,"danger":2400,"mode":0,"alert":0,"rssi":-92,"snr":7.5,"age":0}'
```

The reply carries the device key to activate on the site. Add `"age":120` to see
SENSOR LOST, `"age":-1` for a receiver that has never heard its sensor.

`2-website/aquaiots.sqlite` created by local testing is throw-away data. It is
git-ignored; delete it before uploading to hosting so the live site starts empty.

Headless Chrome screenshots of a page scrolled to a hash come out blank or with
the header floating mid-image. That is the capture, not the page: check
`scrollY` and element rects through the DOM instead.

**Auto-deploy.** `.github/workflows/deploy.yml` runs on a push to `main` that touches
`2-website/` (or by the "Run workflow" button): `php -l api.php`, then
`SamKirkland/FTP-Deploy-Action` (pinned to the commit of v4.3.5) uploads `2-website/`
over FTPS. It excludes `aquaiots.sqlite*`, `CLAUDE.md` and `README.txt`, and the action
only deletes files it uploaded itself, so the live database is never touched. It is inert
until the repo secrets `FTP_SERVER`, `FTP_USERNAME`, `FTP_PASSWORD` (and optionally
`FTP_SERVER_DIR`) exist; without them it ends green and says it skipped. Reason it is a
workflow and not Hostinger's own Git feature: the site lives in the `2-website/`
subfolder, and Hostinger's Git deploy would publish the whole repo (firmware, notes) with
`index.html` and `.htaccess` in the wrong place. Never put FTP credentials in a file.
Because of this, never edit the live files in Hostinger's File Manager: the next push
overwrites them. Setup steps: `IMPLEMENTATION-GUIDE.md` section 5b.

Git: remote `origin` is https://github.com/jeetmagician/flood.git, branch `main`
(public). GitHub holds the code only; the site is not hosted there. Commits carry
the Co-Authored-By trailer.

When changing the API and the front end together, keep the field names aligned:
the firmware posts `level`, `gap`, `depth`, `danger`, `mode`, `alert`, `rssi`,
`snr`, and `api.php` also accepts the `_mm` suffixed forms of each.

---

## Run it on localhost

PHP is installed on the founder's Mac, so the real backend runs locally:

```
cd /Users/suranjeet/Desktop/Aquaiots/2-website
php -S 127.0.0.1:8000          # stop it with Ctrl+C, or:  pkill -f "php -S"
```

| Link | What it opens |
|---|---|
| http://127.0.0.1:8000/index.html | Start screen: Client and Administrator sign-in |
| http://127.0.0.1:8000/index.html#admin | Administrator board (sends you back to sign-in without an admin session) |
| http://127.0.0.1:8000/index.html#client | Client dashboard (same rule, needs a client session) |
| http://127.0.0.1:8000/index.html#how | The public story: how it works, why Aquaiots |
| http://127.0.0.1:8000/index.html#founder | Founder section |
| http://127.0.0.1:8000/index.html#contact | Contact: phone, address, social |

The server stops when the terminal or session that started it ends; start it again
with the command above. The local `aquaiots.sqlite` already holds the founder's test
administrator (made through the setup form; the credentials are deliberately not
written here) and a couple of simulated nodes. It is throw-away and git-ignored.

A receiver on the desk cannot reach `127.0.0.1`. To bench-test the real firmware
against the Mac, run `sudo php -S 0.0.0.0:80` in `2-website/`, set `CLOUD_HTTPS 0`
and `CLOUD_HOST` to the Mac's LAN IP in the cloud sketch, and allow it through the
Mac firewall. The firmware only speaks port 80 or 443.

---

## Firmware: which file to flash

- **ESP8266 receiver: `node_receiver_8266_cloud.ino`.** This is the one for the
  website: local dashboard, LED, siren *and* the uploader that produces the device
  key. `node_receiver_8266_offline.ino` is the same without the uploader, for a desk
  test with no WiFi or website, or a site with no internet. It never shows a key and
  never reaches the site.
- **ESP32 sender: `node_sender.ino`.** A separate board at the water.
- The `.txt` copies in `1-firmware/txt/` are byte-identical to the `.ino` files.

Arduino IDE, one-time: add `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
to Additional Boards Manager URLs and install **esp8266 by ESP8266 Community**; install
**LoRa by Sandeep Mistry** from Library Manager. WiFi, web server, HTTP client, secure
WiFi and SPI come with the board package. Board: *NodeMCU 1.0 (ESP-12E)* or *LOLIN
(WEMOS) D1 mini*. ESP32 sender: use the board settings in Hard constraints above.

Before flashing the cloud receiver edit `STA_SSID`, `STA_PASS` (2.4 GHz only) and
`CLOUD_HOST` (the domain only, no `https://`); `CLOUD_HTTPS 1` needs SSL on the host.
Wiring is in the sketch's header comment; the siren relay stays active-low on D4.

After flashing: join `FloodNode-01` (password `flood1234`), open `192.168.4.1`, read the
eight-character key in the blue bar, activate it on the site. Recommended order: flash
the offline receiver first for the desk test, then the cloud one.

**What the receiver sends** every 2 s (`UPLOAD_SEC`), and immediately when an alert
starts. That interval is the website's whole delay: sensor to receiver is 0.5 s, the
dashboard polls every 2 s, so a change shows within about 2-5 s (it was 25 s):

```
POST https://<CLOUD_HOST>/api.php?a=ingest
Content-Type: application/json
X-Token: AQI<chip id><flash id>
{"level":1840,"gap":1160,"depth":3000,"danger":2400,"mode":0,"alert":0,"rssi":-92,"snr":7.5}
```

**Why a 2 s upload does not endanger the siren.** The danger count increments per
*received* packet and only resets when the level falls below the clear mark, so
packets missed during an upload cannot reset it. The two real risks are handled in
the sketch: (1) a TLS handshake takes 1-3 s with the radio deaf, so ONE connection is
kept alive (`setReuse`, a global `WiFiClientSecure`) and later uploads take a few
hundred ms; (2) a slow or dead server must not keep the radio deaf, so the timeout is
2.5 s (`UPLOAD_TIMEOUT_MS`) and after a failed upload it waits `UPLOAD_RETRY_SEC`
(10 s). The reply is read into a fixed `char` buffer through `writeToStream`
(`ReplyBuf`), not `getString()`, because a `String` every 2 s would fragment the heap.
This keep-alive code has been compiled and run against mock Arduino classes on a PC
only; it MUST be desk-tested on a real board (see Open items).

Reply: `{"ok":true,"code":"K7M2-9QXA","paired":false,"sensor":true}`. The site decides
the state from this: `alert` = 1 is DANGER, level at 85% of the danger mark is
RISING, no upload for 180 s is OFFLINE, and `age` (below) decides SENSOR LOST. The
siren never depends on any of it.

**`age`: how a dead sensor is noticed.** The receiver adds `"age":N` = seconds since
it last heard the *sensor* over LoRa (`-1` = never). The sender transmits every
500 ms, so a healthy sensor is heard twice a second. Server rule (`SENSOR_LOST_SEC`,
30, at the top of `api.php`): if `age` is `-1` or above it, the upload only marks the
receiver online and `devices.sensor_ok = 0`; **no reading row is stored**, so the
chart draws no flat line and the stale level is never passed off as live. A payload
with no `age` (older firmware) counts as fresh, so old receivers keep working.
Columns `devices.sensor_ts` / `sensor_ok` / `lv_*` are added to an older database
automatically on first request (`PRAGMA table_info` / `SHOW COLUMNS`, then
`ALTER TABLE`).

**Three places the data lives (all in `api.php`):**
1. `devices.lv_*` + `depth_mm`/`danger_mm`/`mode`: the LATEST reading, updated on every
   fresh upload. Both portals read the current state from here.
2. `readings`: the live buffer, one row per upload, trimmed to `LIVE_KEEP_SEC` (2 h).
   It feeds only the "Live" chart (last `LIVE_CHART_SEC` = 30 min, thinned to ~150
   points by the server).
3. `history`: ONE row per device per 5 minutes (`slot = floor(ts/300)`, `HISTORY_SLOT_SEC`;
   `UNIQUE(device_id, slot)` + `INSERT OR IGNORE`/`INSERT IGNORE`), kept `KEEP_DAYS`
   (7), about 2,016 rows per device. It feeds the "7 days" chart, the Records card and the PDF
   reports. An alert upload inside a slot sets that slot's `alert` and keeps its highest level, so
   an alert is never lost to a calm first reading. A year is ~105,000 rows per device, so set `KEEP_DAYS = 365` if month/year reports should mean anything.
   `MIN_GAP_SEC` is now 1 so a 2 s receiver is never throttled. Pruning runs on about
   1 in 30 uploads. Nothing is stored while the sensor is lost, so history has a gap
   rather than a fake flat line.

---

## Build log: everything made so far

Starting point: `api.php`, `index.html` (landing + console) and `admin.html` already
existed and matched this file. In order, the work was:

1. **Fixed `index.html` basics.** It had no doctype, charset or viewport; client-typed
   node names were inserted as HTML (now text); the hero wave ignored reduced-motion.
2. **Merged the site into one page.** Admin portal moved into `index.html`;
   `admin.html` became a redirect. One backend fix: `logout` / `admin_logout` now
   unset only their own session key (they used to destroy the whole session).
3. **Redesigned to a professional, futuristic look.** Dark instrument panel, grid
   hero with a live sample gauge (labelled "Sample data"), animated wave, four-step
   chain, specs, six feature cards, founder, contact, footer, mobile menu, show/hide
   PIN, busy buttons, admin board with search and All/Alert/Offline filter.
4. **Two-door start screen.** `#login` shows the Client and Administrator cards;
   `#client` and `#admin` are guarded by the server session; story pages moved behind
   the menu. First run turns the Administrator card into the one-time setup form.
5. **Logo.** The founder's full logo, background removed, in the top bar and start
   screen; the drop alone as favicon.
6. **Water tank / River view filter** on the client card and in the dashboard header
   (see The website). Tested all four client-by-view combinations.
7. **Founder photo, social icons, contact.** Photo embedded in the Founder section;
   Facebook, Instagram and globe icons with the founder's links; customer care number
   and address in a Contact section and in the footer of every view.
8. **Circuit-board background and light/dark switch** (see Conventions). About 90
   hard-coded colours became tokens. Fixed the chart gradient (it ignored the line
   colour). Toggle verified to switch, persist across reload and recolour the wave.
9. **Docs.** `README.txt`, `START-HERE.txt`, this file and its copies updated;
   firmware copied to `1-firmware/txt/`.
10. **Sensor-lost detection (end to end).** Cloud receiver now uploads `age`;
    `api.php` stores no reading and flags `sensor_ok = 0` when the sensor is unheard;
    both portals show it (see "age" above). Tested on the existing old-layout database:
    the migration ran; an old-style payload counted as fresh; age 1 stored a reading;
    age 120 and age -1 stored none and gave state `nosensor`; age 0 recovered; a brand
    new receiver with age -1 registered, returned a key and stored no readings; the
    admin response held no readings. The dashboard text was read back through a
    headless browser. The new firmware line was compiled with format checking on a PC
    (not for the board) and printed the right JSON for never / 5 s / 60 s / 0 s.
11. **`IMPLEMENTATION-GUIDE.md`** written: the full build, test and hand-over path.
12. **Live data, history and PDF reports (2026-10-05).** Founder reported a ~25 s delay,
    asked for a record every 30 minutes kept 7 days, and PDF export by day/month/year
    for client and admin. Done: receiver uploads every 2 s over one kept-alive TLS
    connection with failure backoff and no `String`; `api.php` split into latest /
    live buffer / half-hour history, with migration and pruning; dashboard polls every
    2 s, Live | 7 days chart, export card; `AqPdf` writer; admin status PDF. Admin
    readings export deliberately NOT built (see Security decisions). Tested against the
    real PHP backend and a real browser: 40 uploads 1.1 s apart showed instantly (age
    0 s) and wrote 40 live rows but 1 history row; 4 planted 9-day-old history rows
    were pruned; export refused without a session, for another client's device, and
    for a bad period; the admin board worked with `readings` and `history` renamed
    away; the page generated day and month PDFs and an admin PDF (valid xref, logo,
    multi-page, empty-period) that were rendered and read; the sensor-lost, new-device
    and old-firmware paths still pass. Not tested: the new firmware on a board.
13. **Auto-deploy.** Added the workflow above and the setup guide (5b). Pending on the founder: create the Hostinger FTP account and add the three GitHub secrets.
14. **Git.** Repository created and pushed to https://github.com/jeetmagician/flood
    (public). Commits so far: `909639a` first import, `1e38ea4` two-door site and
    brand, `89998aa` docs and `.txt` firmware, `9b371c1` texture and theme switch,
    `7d65c58` run/firmware guide and build log, `b289116` sensor-lost fix and guide; the live-data / history / PDF work follows.

**Tested against the real PHP backend (via curl and a headless browser):** admin
setup once then refusal, admin login and wrong password, no data without a session,
device ingest creating a device and returning its key, client activation, client login,
identical message for wrong PIN and unknown key, node list and history, admin board
containing no readings, tank/river filtering, theme toggle persistence.

**Not tested:** real ESP hardware and the firmware as a whole, the six-wrong-PINs
lockout, the 5 s post throttle, 90-day pruning, MySQL, real cPanel hosting, the phone
layout on a real phone, and the tank view, admin board and story page in light mode.

15. **Admin sees readings; 5-minute records; PDF header (2026-10-06).** Founder asked that the
    administrator see every registered client ID with River/Tank and the same detail as the client,
    that data be recorded every 5 minutes with date, time, level and alerts for 7 days, downloadable
    as a PDF with logo, founder name and contact details on top, in both portals. Done as described
    above (history slot 300 s, alert upgrade in ingest, `device_for_reader`, admin node rows and detail
    panel, shared `Recs()`, PDF header and alert events). Tested on a scratch copy against the real PHP
    backend and a real browser: admin and client reads, a client refused another client's node,
    unauthenticated read refused, 2,000 seeded rows, alert filter, per-day filter, both PDFs downloaded
    and rendered, no page errors. Not tested: a real phone, light theme, MySQL.

**Never recorded in any file:** the admin's email and password, and any client PIN.

---

## Open items

- No self-service PIN reset. A client who forgets theirs reads the device key
  off the receiver, proving possession, and the admin resets it by hand.
- 433 MHz power and duty-cycle limits under India's WPC low-power rules have not
  been verified. India's usual commercial LoRa band is 865–867 MHz. This matters
  before any commercial deployment, not for prototyping.
- No SMS fallback. Worth adding (SIM800L on the receiver) for sites with no
  broadband, where an alert text beats no alert at all.
- **The 2 s keep-alive uploader has not run on a real ESP8266.** Desk-test it before
  trusting it: Serial Monitor should show `cloud 200` and not stall, the local page at
  192.168.4.1 must stay responsive, the siren must still trigger at the danger mark,
  and heap should stay steady for an hour. If it misbehaves, raise `UPLOAD_SEC` (5 is
  safe) or unplug the Wi-Fi to confirm the siren is unaffected. A host that closes idle
  connections is handled: the next upload reconnects.
- **Hosting load grows with devices:** one device is about 0.5 requests/s. A few dozen
  are fine on shared hosting; for hundreds raise `UPLOAD_SEC` or move to a VPS.
- **Retention is 7 days as requested**, so Month and Year reports only ever contain the
  last 7 days. Raise `KEEP_DAYS` (365 costs ~105,000 rows per device) if real monthly or
  yearly reports are wanted.
- History records the FIRST fresh reading of each 5-minute slot (raised to the highest level
  if an alert arrives inside it), so a calm-level peak between two samples is not recorded.
- A receiver flashed with the *old* sketch still works but cannot report `age`, so a
  dead sensor on it still looks live. Reflash the cloud receiver to get SENSOR LOST.
- A WiFi-setup box on the receiver's own page (name, password, Connect) so the WiFi can
  change per site without reflashing has been proposed, not built. It would not break
  the rule that the *website* never asks for a WiFi password: it would stay on the device.
- No optional "Site name" per client yet. The `clients.label` column exists and
  nothing fills it; it would let the admin see which site a client ID is without
  seeing readings.
- Phone layout is unverified on a real phone; headless Chrome cannot go below
  about 500 px wide.
- All firmware changes have been compiled only against mocks on a PC; none has been flashed to a board.
- The lockout (six wrong PINs), device post throttle, 90-day pruning, MySQL mode
  and real cPanel hosting are untested.
