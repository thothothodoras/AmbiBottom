# AmbiBottom — WLED usermod (ESP32-S3)

Makes an ESP32-S3 running WLED 0.15.x look like a Philips Hue bridge to a
Philips Ambilight TV, receives the TV's bottom colours (Hue Entertainment,
DTLS) and shows them on the strip WLED drives. No Linux box needed ("Mode A"
in `docs/ARCHITECTURE.md`).

**Status, 2026-11-12: compiles and links; the logic is tested on Linux; it has
never run on an ESP32.** Everything in "Not verified" below is still open.

## WLED 16.0.x (v0.36, 2026-11-17) — compiles and links, not yet on the device

The AtomS3 runs stock WLED 16.0.1 since 2026-11-17, so the usermod was ported:

```
git clone --depth 1 --branch v16.0.1 https://github.com/wled/WLED.git
/path/to/ambi-bridge-cpp/firmware/wled-usermod/install16.sh /path/to/WLED
cd /path/to/WLED && pio run -e atoms3_ambibottom
```

- **Registration:** WLED 16 has no `usermods_list.cpp`; a usermod is a library listed in
  `custom_usermods` that calls `REGISTER_USERMOD()` (`AmbiBottom/ambibottom_wled16.cpp`,
  `AmbiBottom/library.json` with `"libArchive": false`). `install16.sh` writes only
  `platformio_override.ini` (git-ignored by WLED); WLED's own files are not touched. The
  entry must be written `AmbiBottom = symlink://...`: WLED 16's `load_usermods.py` cannot guess
  the name from a `symlink://` path, and without the name the usermod gets no `wled.h` include path.
- **Platform:** WLED 16's default platform (Tasmota platform-espressif32 2024.06.00, Arduino
  2.0.18) is built with `CONFIG_MBEDTLS_TLS_DISABLED` - no TLS / DTLS at all, so
  `mbedtls_ssl_set_hs_psk`, `mbedtls_ssl_conf_dtls_cookies` etc. do not exist. The environment
  uses the Espressif platform WLED 16 itself names as the alternative: `espressif32@ ~6.13.0`
  (arduino-esp32 2.0.17, ESP-IDF 4.4.7), whose mbedTLS has DTLS and PSK.
- **Pins as wired on 2026-11-17:** data `DATA_PINS=38`; G41 = our link button, G35 status LED.
  `WLED_DISABLE_ALEXA` (Espalexa answers the same `/description.xml` and `/api`) and
  `WLED_DISABLE_HUESYNC` (WLED as a client of a real Hue bridge: not needed).

Measured on the build (same platform and flags, with and without the usermod; env
`atoms3_baseline` is the one without):

| | flash (app slot 2 097 152) | static RAM (data + bss, of 327 680) |
|---|---|---|
| with AmbiBottom | 1 749 477 bytes, 83.4 % | 89 432 bytes |
| without | 1 498 641 bytes, 71.5 % | 88 564 bytes |
| **AmbiBottom adds** | **250 836 bytes** (0.15: 248 616) | **868 bytes** |

The OTA slot has the same size (`WLED_ESP32_8MB.csv`: app0 / app1 2 MB each), so updates over
Wi-Fi fit. **Free heap at run time: not measured yet** (stock WLED 16 on the device: 205 KB).

**Getting it onto the AtomS3** (it replaces the stock WLED; the user agreed 2026-11-17):
WLED's own update page (Config -> Security & Updates -> Manual OTA update) with the app image.
WLED 16 refuses an image with a different release name (`ESP32-S3_8MB_AmbiBottom` vs the
stock one) unless "Ignore firmware validation" is ticked (`skipValidation=1`). Not tried yet:
moving from the Tasmota-platform build to the Espressif-platform build by OTA (both are
ESP-IDF 4.4.x). If it does not boot: flash over USB from Windows (bootloader 0x0, partitions
0x8000, app 0x10000) and, to go back, the stock image from install.wled.me.

**Tests for the device** (both first checked against `tests/esp_app_sim.cpp`):
`tools/esp_hue_diff.sh <ESP IP>` = the TV's 26 REST requests to the Linux bridge (5 lamps) and to
the ESP, replies diffed - against the simulator 21 of 26 identical, the other 5 differ only in
the identity (name `AmbiBottom` vs `FakeBridge-A45E01`, IP, gateway, the URL in
description.xml); `tools/esp_stream_test.sh <ESP IP>` = pairing + DTLS-PSK stream from
`tools/sender` (SENDER_HOST) for 20 s with `/json/info` free heap read 4 times a second.

## What is where

| Path | What | Runs on |
|---|---|---|
| `AmbiBottom/usermod_ambibottom.h` | the WLED side only: web routes, SSDP socket, NVS, button, status LED, pixels, settings | ESP32 |
| `lib/AmbiBottomEsp/src/ambi_bottom_app.*` | pairing store, state, glue to the core | ESP32 **and Linux tests** |
| `lib/AmbiBottomEsp/src/ambi_dtls.*` | DTLS-PSK stream server (BSD sockets + mbedTLS 2.28 / 3.x) | ESP32 **and Linux tests** |
| `../../shared/ambi_core/` | colour pipeline, HueStream parser, Hue v1 REST logic (`hue_core`), TV-lock rules, settings limits, SSDP texts | ESP32 **and the Linux bridge** |

One source: the build takes `shared/ambi_core` and `lib/AmbiBottomEsp` from
this repository through `symlink://` library entries; nothing is copied into
WLED. The Linux bridge compiles the very same `shared/ambi_core` files.

## Build

```
git clone --depth 1 --branch v0.15.5 https://github.com/wled/WLED.git
/path/to/ambi-bridge-cpp/firmware/wled-usermod/install.sh /path/to/WLED
cd /path/to/WLED && pio run -e esp32s3dev_8MB
```

`install.sh` adds five lines to `wled00/usermods_list.cpp` (WLED 0.15 has no
other way to register a usermod) and writes `platformio_override.ini` with the
environment `esp32s3dev_8MB`. WLED 0.15.5 itself has no environment of exactly
that name (only `esp32s3dev_8MB_opi`, for boards with PSRAM); ours is the same
without PSRAM, for the AtomS3 Lite (ESP32-S3FN8).

Measured on 2026-11-12 (WLED 0.15.5, arduino-esp32 2.0.9, mbedTLS 2.28.1):

| | flash (app slot 2 097 152) | static RAM (data + bss) |
|---|---|---|
| with AmbiBottom | 1 649 845 bytes, 78.7 % (v0.33; v0.32: 1 645 805) | 57 004 bytes |
| same environment without it | 1 397 189 bytes, 66.6 % | 56 316 bytes |
| **what AmbiBottom adds** | **248 616 bytes** (incl. the DTLS/PSK server parts of mbedTLS it pulls in) | **688 bytes** |

Library sizes inside it: `ambi_core` 97 KB code, `AmbiBottomEsp` 34 KB. The
OTA slot has the same size, so updates over Wi-Fi fit. **Heap at run time was
not measured** (no hardware): one DTLS session in this SDK allocates two
16 KB record buffers plus the handshake state, roughly 40 KB while the TV
streams; the stock WLED build has about 200 KB free on this chip.

## Design choices

**Port 80.** The TV fetches `http://<ip>/description.xml` and then talks to
`http://<ip>/api/...`, with no port in the URL — port 80 only. WLED's web
server lives there. So the fake bridge has no server of its own: the usermod
registers `/description.xml` and `/api` on WLED's `AsyncWebServer`. WLED's
Alexa emulation (Espalexa) answers exactly those paths (it also plays a Hue
bridge), so the build needs `-D WLED_DISABLE_ALEXA`; the usermod refuses to
compile without it. Alexa voice control of WLED is therefore not available in
this build. WLED's own pages and its JSON API (`/json`, `/win`, `/settings`)
are untouched.

**Pairing only with the button.** `POST /api` is answered with error 101
unless the link button (GPIO41 on the AtomS3 Lite) was pressed in the last
30 s. Hold it 10 s to forget every paired TV. `POST /ambibottom/pair` does the
same as a press, for a device mounted out of reach; it obeys WLED's settings
PIN.

**State in NVS.** Pairing (usernames + stream keys) and the groups are stored
in NVS, namespace `ambibottom`, not in WLED's `cfg.json`: they survive a
reboot, an OTA update and a WLED "factory reset" of the configuration, and
they do not appear in a WLED config backup (the keys are secrets). A full
flash erase removes them. The usermod's own settings (below) are ordinary
WLED settings in `cfg.json`.

**Same behaviour as the Linux bridge (v0.31/v0.32).** Group ids from 1, reuse
of a group with the same type + lights, real DELETE, an empty placeholder for
ids that are gone, error 1 for unknown users, the lost-pairing log, `/lights/N`
always HTTP 200, the open DTLS session ended at once on `stream.active=false`,
and the "TV locked to LightGroup" state: all of it is the same code
(`hue_core`, `tv_lock`), not a second implementation. The lock is shown on
WLED's Info page, at `GET /ambibottom/status` (`tv_dtls_lock`) and as a red
status LED.

**Output.** The usermod writes linear LED values with WLED's realtime API
(`realtimeLock` + `setRealtimePixel`) and switches realtime gamma correction
off while it drives the strip. When the stream stops: hold 1 s, fade 1 s,
black for 30 s, then the strip goes back to WLED's own state — set WLED's
boot preset to "off" if the strip should stay dark.

**Two current limits.** The pipeline's own (setting `budgetMa`, default 450,
never above the build's ceiling `AMBI_MAX_BUDGET_MA` = 900) and WLED's ABL as
the second net. The sample build sets `ABL_MILLIAMPS_DEFAULT=450` together
with `-D MA_FOR_ESP=0` and `-D LED_MILLIAMPS_DEFAULT=45` (v0.33; 45 since v0.34): WLED 0.15
takes `MA_FOR_ESP` (120 mA) off its limit "for the ESP", which here has its
own USB port, so without that flag 450 meant 330 mA and WLED dimmed
everything the pipeline let through. With the flags, enter **the same number
as `budgetMa`** in LED Preferences → Maximum current. The ABL is the ONLY
limit whenever the usermod is not driving the strip (boot preset, effects,
after the stream ends): the usermod puts a warning on WLED's Info page when
it is off or above the ceiling + `MA_FOR_ESP`. Build flags are defaults for a
fresh config; a saved `cfg.json` keeps its own values.

## Settings (WLED → Config → Usermods → AmbiBottom)

| Setting | Default | Note |
|---|---|---|
| `enabled` | on | |
| `lamps` | 3 | 3 or 5 virtual lamps; reboot to apply |
| `tvIp` | empty | only this address may pair; reboot |
| `bridgeMac` | empty | empty = `00:17:88` + last 3 bytes of the ESP's MAC. Whether the TV insists on the Philips prefix is **not measured**; reboot |
| `linkPin` / `statusPin` | 41 / 35 | AtomS3 Lite button and RGB LED; -1 = none; reboot |
| `budgetMa` | 450 | hard limit for the strip. Not measured for your supply. At most 900 (build flag `AMBI_MAX_BUDGET_MA`) |
| `maPerLed` | 45 | full white per LED, measured on the reference strip (v0.34; was 60 = spec) — measure yours |
| `maScaleRPct` / `GPct` / `BPct` | 100 | v0.33: what each colour channel draws, in % of a third of white (10–300). 100 = as before; **not measured** (how to: `docs/HARDWARE.md`) |
| `brightnessPct` | 100 | applied before the limit |
| `riseMs` | 120 | how fast a jump to bright may happen |
| `peakEmphasisPct` | 50 | how much the limiter favours the brightest spots |
| `layout` | tv | `tv`, `inset` or `custom` |
| `lampPositions` | empty | for `custom`: one number 0..1 per lamp, e.g. `0,0.5,1`; `-1` = lamp not shown |
| `dtlsLimitS` | 3 | how long the TV waits for the stream handshake (from the TV's decompiled code, **not measured**) |

Strip length and data pin are WLED's own LED settings.

## Status LED

red = TV locked (restart the TV) · magenta = TV streams from an old session ·
blinking blue = pairing open · yellow = the TV asked to pair (press the
button) · green = streaming · blinking amber = not paired · dim white = idle.

## Not verified (needs the hardware)

1. That it boots and runs at all on an AtomS3 Lite.
2. DTLS on the ESP: the handshake with the real TV, the 12 KB task stack, heap
   headroom, frame rate. (The same server file passes the Linux DTLS tests
   against a real mbedTLS client, with mbedTLS 3.6; the ESP has 2.28.)
3. The status LED: `neopixelWrite()` takes an RMT channel next to the ones
   WLED's LED driver uses. If the strip or the LED misbehaves, set
   `statusPin` to -1.
4. The web handlers run in the AsyncTCP task (8 KB stack in WLED's S3 build);
   the largest reply is the full state dump of `GET /api/<user>`.
5. The TV accepting the default bridge MAC and finding the bridge by SSDP from
   an ESP (multicast through `AsyncUDP`).
6. An address change after a DHCP renewal: the bridge identity is fixed when
   the network first comes up; reboot after an address change.
7. Timing on a busy ESP: `/lights/1` must answer within 300 ms ten times in a
   row, and the DTLS handshake within `dtlsLimitS`.

How the logic was tested without an ESP: `tools/esp_app_test.sh` (30 checks)
runs `tests/esp_app_sim.cpp`, a Linux process built from the same
`ambi_bottom_app.cpp` + `ambi_dtls.cpp` + `shared/ambi_core`, and talks to it
like the TV (curl + a real DTLS client); `ctest` runs `esp_dtls`, `hue_core`
and `shared_pipeline`.

## License

This directory is under the EUPL-1.2, like WLED, because the usermod is
compiled into WLED. `shared/ambi_core` is MIT (see the repository's
`LICENSE` and `docs/LICENSING.md`).

## v0.37 (2026-11-18) — the page, Start / Stop, Demo (alpha, simulator only)

- `http://<ESP>/ambi` = the Linux monitor page, made by `tools/make_esp_page.py` from
  `src/monitor_server.cpp` and stored gzipped (`AmbiBottom/ambi_page_gz.h`, 49.5 KB). Re-run the
  script after any change of the Linux page (`--check` tells).
- `/ambi/api/{state,service,pair,login,settings/strip,demo}`; POSTs need WLED's settings PIN when
  one is set. Strip settings from the page are saved in NVS (`strip`) and copied to the usermod's
  fields (cfg.json).
- Boots **stopped** (the strip is WLED's). Start: TV colours / black / Demo until Stop. Demo = a
  WLED effect (default: a calm one at random, 25 %, speed 60). Stop restores WLED's effect, speed,
  intensity, palette, brightness and "Force max brightness" from before Start.
- Build: flash 1 821 293 bytes (86.8 %), static RAM 89 728. Not yet run on the device.
- `MA_FOR_ESP=0` in this build: WLED's "Maximum current" counts the LEDs only (bench on the
  Atom's 5 V: 330; strip on its own USB: 450).
- **On the device 2026-11-18** (bench AtomS3, OTA from stock WLED): works after four fixes - form-encoded
  request bodies (`takeBody`), `-D WLED_USE_SHARED_RMT` (WLED 16's NeoEsp32RmtHI driver waited 10 s per
  `show()` on the Espressif platform), DTLS task pinned to core 0 / priority 3 / 50 ms back-off on errors,
  the page's effect list loaded later with retries. Details and numbers: README.md row v0.37.
- An Atom first set up with stock WLED has WLED's Button 0 on GPIO 41: the usermod's link button is then
  off (`linkPin` -1). Pair from the page, or set Button 0 to -1 in LED Preferences and `linkPin` to 41.
