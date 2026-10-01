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

**The admin portal cannot see water readings.** `admin_clients` never touches
the readings table. This is a promise that can be made to customers in writing;
it is one line away from being broken, so do not "improve" that query. Client
PINs are hashed and never retrievable.

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
receiver is one POST (use `"mode":1` for a tank; wait 5 s between posts, the
server throttles faster ones):

```
curl -X POST 'http://127.0.0.1:8000/api.php?a=ingest' \
  -H 'X-Token: AQIDEMO000000001' -H 'Content-Type: application/json' \
  -d '{"level":1840,"gap":1160,"depth":3000,"danger":2400,"mode":0,"alert":0,"rssi":-92,"snr":7.5}'
```

The reply carries the device key to activate on the site.

`2-website/aquaiots.sqlite` created by local testing is throw-away data. It is
git-ignored; delete it before uploading to hosting so the live site starts empty.

Headless Chrome screenshots of a page scrolled to a hash come out blank or with
the header floating mid-image. That is the capture, not the page: check
`scrollY` and element rects through the DOM instead.

Git: remote `origin` is https://github.com/jeetmagician/flood.git, branch `main`
(public). GitHub holds the code only; the site is not hosted there. Commits carry
the Co-Authored-By trailer.

When changing the API and the front end together, keep the field names aligned:
the firmware posts `level`, `gap`, `depth`, `danger`, `mode`, `alert`, `rssi`,
`snr`, and `api.php` also accepts the `_mm` suffixed forms of each.

---

## Open items

- No self-service PIN reset. A client who forgets theirs reads the device key
  off the receiver, proving possession, and the admin resets it by hand.
- 433 MHz power and duty-cycle limits under India's WPC low-power rules have not
  been verified. India's usual commercial LoRa band is 865–867 MHz. This matters
  before any commercial deployment, not for prototyping.
- No SMS fallback. Worth adding (SIM800L on the receiver) for sites with no
  broadband, where an alert text beats no alert at all.
- **The website cannot tell the sensor has died.** The receiver uploads its last
  known level every 25 s even when no LoRa packet has arrived, so a dead sender
  looks online and normal on the site (the siren still latches locally). Fix needs
  a firmware change (send a stale flag, or skip the upload) and a matching
  `api.php` change. Not done; firmware changes are suggestions for a human.
- **`readReply()` in the cloud receiver uses Arduino `String`**
  (`http.getString()`), against the fixed-`char`-buffers rule, on every upload.
  Replace with a bounded read into a `char` buffer.
- Until its first radio packet the receiver defaults to mode river, so a brand-new
  tank node can briefly look like a river.
- No optional "Site name" per client yet. The `clients.label` column exists and
  nothing fills it; it would let the admin see which site a client ID is without
  seeing readings.
- Phone layout is unverified on a real phone; headless Chrome cannot go below
  about 500 px wide.
- The lockout (six wrong PINs), device post throttle, 90-day pruning, MySQL mode
  and real cPanel hosting are untested.
