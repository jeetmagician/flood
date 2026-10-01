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
refuses forever. There is no password reset and no second admin.

**Database errors go to the PHP error log, never to the browser** — they leak
table names and paths. Every response path is `fail('Server error.', 500)`.

---

## Conventions

- `api.php` is deliberately one file with no dependencies, no composer, no
  framework. It must keep running on ₹1,500/year cPanel shared hosting with
  nothing but PHP 8.1. Do not introduce a build step, a package manager or a
  framework.
- SQLite by default; MySQL is a five-constant switch at the top. The schema is
  created by the file itself on first request.
- The front end is plain ES5-era JavaScript in one `<script>`, no framework, no
  bundler, no npm. Pages are self-contained single files.
- Design tokens live in `:root` in each page. Dark single-theme by intent.
  Display face Bricolage Grotesque, body IBM Plex Sans, mono IBM Plex Mono.
  Accent `--cy: #37C6E8`. Status colours are separate from the accent.
- All distances are millimetres, integer, everywhere — firmware, wire format,
  database, API. Metres appear only in rendered text.
- Comments in the firmware explain *why*, especially where a line looks wrong
  but is load-bearing. Keep them when editing.

---

## Working on this

The firmware cannot be compiled or flashed by an agent — it needs the Arduino
IDE and a physical board. Changes to `1-firmware/` are suggestions for a human
to flash and test.

`index.html` and `admin.html` cannot be previewed meaningfully without PHP
behind them; opened as plain files, every sign-in fails. That is expected, not a
bug. Test against a real PHP server (`php -S 127.0.0.1:8000` in `2-website/` is
enough) rather than a static preview.

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
