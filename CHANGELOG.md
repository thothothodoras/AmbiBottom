# Changelog

Versions before 0.36 were the Linux bridge only (Mode B). Firmware images are
attached to the GitHub releases, not stored in the repository.

## 0.50
- **Calibration slots**: keep up to three named white balances on the board (e.g. day / night)
  and load one with a click (Settings → Camera calibration → Slots).
- **WLED's Maximum current** follows the limiter's own formula (the strip's share = the page's
  limit minus the board's share), so a page limit below 50 mA is no longer raised to 50 in WLED.
  The value also includes WLED's `MA_FOR_ESP` (0 in our builds, `-D MA_FOR_ESP=0`; 120 mA in a
  build without the flag), and is never below `MA_FOR_ESP` + 1 (0 = WLED's limiter off).
- WLED's Maximum current set by hand above the ceiling is corrected back in WLED, not just adopted
  on the page; WLED's power warning counts the board's share. Saving the Usermods page updates
  WLED's value too.
- Diagnosis compares WLED with the value the page gives it (no false "page limit ≠ WLED" warning
  that led to raising the limit); the power meter's budget is the strip's share.
- Live camera calibration only steps on a new frame (not while the tab is hidden or the video is
  frozen) and stops after 40 steps; a stuck state request no longer stops polling (3 s timeout).

## 0.49.1
- Camera calibration fixes: one step at a time (a slow save could let a second step reuse the
  same photo and push the white balance much too far); each photo is used once; smaller, damped
  steps (a channel drops by at most a third per step); a warning when a channel reaches the 0.2
  floor; **Undo** (back to the white balance before the calibration); with a photo the button no
  longer stays on "Pause" while waiting - it says what is missing (e.g. tick "The TV shows
  white / grey" when JointSpace is off); the White balance fields show the new values.

## 0.49
- **Camera calibration** (Settings): point a phone at the wall, drag two boxes onto the side and
  the bottom glow, and the page corrects the white balance until they match (live camera over
  https, or a photo).
- White balance **keeps pure colours** (a pure blue is no longer dimmed by it).
- **Shared USB port**: the board's own current can be counted against the strip's limit.
- Greek page as a small patch on the English one (~44 KB less flash); phone header fixed; the
  output row shows the chip and device name; the page stops polling while its tab is hidden.
- Fixed: JointSpace could stay off after one low-memory moment; the settings index hid the
  Settings tab.

## 0.48
- **Black box** for crashes: a record that survives a reset (failed memory requests with their
  size and code addresses, the lowest free memory and what was going on), shown in Diagnosis;
  the page's API answers "busy" below a memory floor instead of pushing the board over.
- The TV remote and the IR test are **removed** again (opening a TLS connection per key press
  could hang the TV's API until a power cycle).
- **Use JointSpace** switch (off = no connection to the TV, ~45 KB more free memory).
- **White balance** (R / G / B gains) in the strip settings: the WS2812's white is cold next to
  the TV's own LEDs.
- The PIN dialog opens from every tab.
- Live tab: current first, then the wear bar, then the strip card; details behind arrows.
- A **classic ESP32 4 MB** build (English page only).
- JointSpace waits until the Hue stream has settled and only connects with enough free memory.

## 0.46
- **Remote** card on the page: power (on / standby), arrows, OK, back, home, settings, options,
  exit, volume, mute, Ambilight - over JointSpace (the same credentials). **IR** mode (test): the
  AtomS3 Lite's IR LED sends Philips RC6 codes, e.g. to switch the TV on from deep standby.
- An **Ambi** button in WLED's own top bar (`install16.sh` adds it to WLED's UI at build time).
- Settings tab: a side column (bridge card + an index of the cards) and one column of full-width
  cards, so the cards always line up.
- The strip's brightness slider is always visible on the Live tab.
- `tools/jointspace_pair.py` and [docs/JOINTSPACE.md](docs/JOINTSPACE.md): three ways to get the
  JointSpace credentials (the page, Home Assistant, the script).

## 0.45 (first public package)
- **JointSpace pairing from the page**: "Pair with the TV", the TV shows a PIN, type it,
  done (the credentials are saved on the board). Typing existing credentials still works.
- The ESP page is in **English**, with a Greek version (`/ambi/el`; the language button
  switches and the browser remembers it).
- Release package: firmware + bootloader / partitions / boot_app0 for a USB install,
  INSTALL guide, licence (EUPL-1.2 for the whole project).
- Default device name *AmbiBottom* and time zone UTC on a fresh install; all of WLED's
  time zones on the page.

## 0.44
- **Diagnosis** (read-only, 12 checks, copyable report without secrets); **Hue capture**
  (raw TV frames to CSV) for testing TV modes.
- Watchdog on (10 s); WLED's `/json/cfg` needs the settings PIN (CVE-2026-71264 guard).
- Adaptive smoothing of small changes (`smooth_ms`), so the strip moves as calmly as the
  factory Ambilight while scene cuts still pass at once.
- Night limit; PIN dialog; the LED wear log folded under the technical data.

## 0.43
- The Demo exists only when enabled, and switches itself off when the TV streams.
- 🔒 PIN button on the page.

## 0.42
- Soft start (`fade_in_ms`) and soft fade to black (`fade_out_ms`); Start / Stop fade.
- Security card (PIN, OTA lock, AP password, "Hue only from the TV").
- JointSpace paused during a firmware update (more free memory for the upload).

## 0.41
- LED wear log on the ESP (saved to WLED's filesystem), "since the strip was mounted".
- Atom button: pairing or WLED; bridge name; time zone / NTP.

## 0.40
- The page takes over WLED's strip settings: total LEDs, data pin, lamps 3 / 5,
  auto Start, forget TV, restart, event log; current limit kept in step with WLED's.
- Unused WLED features left out of the build (smaller firmware).

## 0.39
- mDNS like a real bridge: the TV finds the board again within ~11 s after a restart.
- Memory and flash shown on the page.

## 0.38
- Links between WLED's page and the Ambi page.

## 0.37
- The page on the ESP (`/ambi`), Start / Stop, Demo, JointSpace on the ESP, the TV is
  asked to reconnect by itself after the board restarts mid-stream.

## 0.36
- First build of WLED 16 with the usermod for the M5Stack AtomS3 Lite.
