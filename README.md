# AmbiBottom

**A fourth side for Philips Ambilight TVs: an LED strip under the TV that follows the picture.**

Philips Ambilight TVs light the wall on three sides (left, top, right). AmbiBottom
adds the bottom. A small ESP32-S3 board running [WLED](https://kno.wled.ge) with the
AmbiBottom usermod pretends to be a Philips Hue bridge; the TV's own
**Ambilight+hue** feature then streams the colours of the bottom edge to it about
25 times a second, and the board shows them on a WS2812B LED strip.

> Not affiliated with, endorsed by or supported by Philips, Signify or TP Vision.
> "Philips", "Ambilight" and "Hue" are their trademarks, used here only to say what
> this works with. You are wiring LEDs to a TV's USB port: read **Safety** first.
> Use at your own risk.

**Status: beta (v0.50).** Runs every day on one TV (Philips 65OLED707/12, Android TV
11) with an M5Stack AtomS3 Lite and a 39-LED strip. Other Philips TVs with
*Ambilight+hue* in their settings should work the same way, but have not been tried.

## What you get

- The bottom of the picture on the wall, in step with the TV's own Ambilight
  (the TV sends three bottom colours; the strip blends between them, and with the
  TV's API credentials (optional, see JointSpace below) the corners match the
  factory sides).
- A web page on the board (`http://<board>/ambi`, English; Greek at `/ambi/el`, works on a phone): live view of all four sides,
  Start / Stop, strip and current settings, device settings, security check,
  one-click **Diagnosis** (with a "black box" that records what ran out of memory before a
  crash), Hue capture for testing, LED wear log, white balance (that keeps pure colours pure), a
  **camera calibration** card (match the strip's white to the TV's sides with a phone photo or
  live camera) with three **calibration slots** on the board, and an **Ambi** button in
  WLED's own top bar.
- A hard current limit (the TV's USB port is small), soft on / off, night limit.
- Everything local: no cloud, no account, no internet needed.

## What you need

| | |
|---|---|
| TV | Philips Ambilight TV with **Settings → Ambilight → Ambilight+hue** |
| Board | An **ESP32 that runs WLED and drives an LED strip**. Tested: **M5Stack AtomS3 Lite** (ESP32-S3, 8 MB flash) + a level shifter. Ready-made WLED controllers (an ESP32 with a level shifter, fuse and screw terminals, often powered over USB-C) do the same job with less wiring; they need a firmware build for their chip (see *Boards* below) |
| Strip | WS2812B, 5 V, 30 LED/m; as long as the TV is wide minus ~8 cm at each end (39 LEDs for a 65" TV) |
| Parts | level shifter (Adafruit Pixel Shifter #6066), resettable fuse 1.1 A hold, capacitor 470–1000 µF / 10 V, 330 Ω resistor, two USB cables |
| Power | two USB ports on the TV (one for the board, one for the strip; use the TV's higher-current port for the strip, often marked 900 mA). One port for both works too: enter the board's own draw on the page (*Device → Power: the ESP's own current from the strip's USB port*) and the strip gets the rest |
| Network | 2.4 GHz Wi-Fi; TV and board on the same network |

Wiring, parts and power: [docs/HARDWARE.md](docs/HARDWARE.md).

### Boards

| Board | State |
|---|---|
| M5Stack AtomS3 Lite (ESP32-S3, 8 MB) | **tested**; `AmbiBottom_..._ESP32-S3_8MB.bin`, page in English and Greek |
| other ESP32-S3 boards / controllers with 8 MB flash | should work with the same firmware if the strip's data pin is one the page offers; untested |
| classic ESP32 controllers with 4 MB flash (e.g. many USB-C WLED controllers, data on GPIO16 / GPIO2) | **builds, untested on hardware**: `AmbiBottom_..._ESP32_4MB.bin`, English page only, WLED's "1.8 MB firmware / 256 KB files" layout (a first install over USB: that layout's partitions file) |
| ESP32-C3 | not yet |

Whatever the board: power the strip from a source that can give the current limit you set
(a TV USB port gives 0.5-0.9 A), and set the limit on the page.

## Safety

- **A current limit is mandatory.** 39 LEDs at full white want ~1.6 A; a TV USB port
  gives 0.5–0.9 A. Set the limit on the page (*Strip LED & current → Current limit*)
  to what your port gives, with margin: 450 mA for a 500 mA port, at most ~810 mA
  for a 900 mA port. The firmware never allows more than 900 mA.
- Keep the **fuse** in the strip's +5 V. It is for the day something else goes wrong.
- **Never join the two USB +5 V lines.** Join only the grounds.
- The TV keeps its USB ports powered in standby: the strip is switched off by the
  software (it fades to black when the TV stops streaming).
- Insulate the board and the joints; do not cover the TV's vents.

## Install (the short version)

Full guide with pictures of each step: **[docs/INSTALL.md](docs/INSTALL.md)**.

1. Put WLED on the board (if it has none): <https://install.wled.me>, then join it to
   your Wi-Fi.
2. In WLED: *Config → Security & Updates → Manual OTA update* → choose
   `AmbiBottom_vX.YY.bin` → tick *Ignore firmware validation* → *Upload*.
3. Open `http://<board IP>/ambi` → *Bridge & settings*: set the LED count, the data
   pin, the current limit, and press **Pair (30 s)**.
4. On the TV: *Settings → Ambilight → Ambilight+hue → Configure*, choose the bridge
   called **AmbiBottom**, select the lamps and give them their positions (bottom-left,
   bottom-middle, bottom-right).
5. Turn *Ambilight+hue* on and press **Start** on the page (or tick *Auto Start*).
6. Set a **settings PIN** and the **OTA lock** in WLED (*Security & Updates*); the
   page's *Security* card shows what is still open.

### JointSpace (optional)

The TV's local API (JointSpace, HTTPS port 1926) gives the colours of its three
factory sides, so the strip's corners match them. It needs a username / password
that the TV hands out once, in a pairing where it shows a PIN on screen:
*Bridge & settings → TV connection (JointSpace) → **Pair with the TV***, then type the
PIN the TV shows. If you already have JointSpace credentials (from another tool), type
them into the same card instead (or take them from Home Assistant): see
[docs/JOINTSPACE.md](docs/JOINTSPACE.md). Without JointSpace the strip works; only the corner
blending is simpler.

## How it works

```
 TV (Ambilight+hue) ──Hue REST + DTLS stream──► ESP32-S3 (WLED + AmbiBottom) ──► LED strip
        └──────────JointSpace HTTPS (optional)───────┘
```

- **Hue bridge emulation**: description.xml, Hue REST v1 (pairing with a button
  press, groups, the TV's lamp checks), Hue Entertainment over DTLS-PSK.
- **Colour pipeline** (`shared/ambi_core`): lamps → LEDs along the strip, the TV's
  lamp positions, corners from JointSpace, current limit with "peaks" that stay
  bright, soft start / fade, adaptive smoothing.
- **WLED usermod** (`firmware/wled-usermod`): web page and API under `/ambi`, the
  strip through WLED's realtime mode, WLED's own effects for the optional demo.
- A **Linux bridge** (`src/`) does the same on a Linux box and sends to any WLED over
  DDP (Mode B, for development and testing).

More: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Build from source

- Firmware: WLED 16.0.x + this usermod, see
  [firmware/wled-usermod/README.md](firmware/wled-usermod/README.md)
  (`firmware/wled-usermod/install16.sh <WLED checkout>`, then
  `pio run -e atoms3_ambibottom`).
- The page: `tools/make_esp_page.py` builds the ESP page (English, with a Greek
  version) from the monitor page in `src/monitor_server.cpp`.
- Linux bridge and tests (Debian / Ubuntu):
  `sudo apt install build-essential cmake pkg-config libmbedtls-dev libcjson-dev`,
  then `cmake -S . -B build && cmake --build build -j && (cd build && ctest)`.
  Page / API tests against a simulator of the firmware: `tools/v037_test.sh`,
  `tools/v041_test.sh` ... `tools/v049_test.sh`.

## Reporting a problem

Open the page → *Bridge & settings* → **Diagnosis** → *Check now* → *Copy report*,
and paste the report into the issue (it contains no passwords or keys).

## Licence

**EUPL-1.2** ([LICENSE](LICENSE)). The firmware is built with WLED, which is EUPL-1.2
too. If you share a built firmware, share the source with it. Details and
third-party code: [docs/LICENSING.md](docs/LICENSING.md).
