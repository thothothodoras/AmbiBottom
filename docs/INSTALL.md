# Installing AmbiBottom

This guide takes you from a bare board to the strip following your TV. It assumes
the hardware is wired as in [HARDWARE.md](HARDWARE.md) (fuse, level shifter,
capacitor, two USB ports on the TV).

What is in the release package (`AmbiBottom-vX.YY.zip`):

| File | What for |
|---|---|
| `AmbiBottom_vX.YY.bin` | the firmware: WLED 16.0.1 with the AmbiBottom usermod, for the **M5Stack AtomS3 Lite** (ESP32-S3, 8 MB) |
| `bootloader.bin`, `partitions.bin`, `boot_app0.bin` | only for a first install over USB with esptool (see the end) |
| `INSTALL.md`, `README.md`, `LICENSE` | this guide, the overview, the licence |

## 1. WLED on the board

If the board already runs WLED 0.15 / 16, skip to step 2.

1. Connect the board to a computer with a USB-C cable.
2. Open <https://install.wled.me> in Chrome or Edge, choose the latest WLED for
   **ESP32-S3 (8 MB)** and install it.
3. The board opens a Wi-Fi network called `WLED-AP` (password `wled1234`). Join it,
   and on the page that opens enter your home Wi-Fi (2.4 GHz). The board restarts and
   joins your network. Find its address in your router, or open <http://wled.local>.

## 2. The AmbiBottom firmware

1. Open the board's WLED page → **Config → Security & Updates → Manual OTA update**.
2. Choose `AmbiBottom_vX.YY.bin`, tick **Ignore firmware validation** (the file says
   "AmbiBottom", not stock WLED) and press **Upload**. Wait about a minute; the board
   restarts by itself. Do not unplug it meanwhile (if the upload is cut, the board
   keeps its old firmware; just try again).
3. WLED's **Info** panel now has a button **Ambi page ↗**. The page itself is
   `http://<board address>/ambi`.

Your WLED settings (Wi-Fi, name) stay. Updates later: the same steps with the new
file; the TV pairing and the settings are kept.

## 3. Strip and power

On the Ambi page → **Bridge & settings**:

1. **Device**: *Total LEDs on the strip* (all the LEDs you have), *Data pin* (where the
   strip's data wire is; GPIO 38 on the AtomS3 Lite header in the reference wiring),
   *Time zone*. Save.
2. **LED strip & current**:
   - *mA per LED at full white*: 40 for the usual WS2812B (39 measured); measure yours if
     you can (HARDWARE.md).
   - *Current limit*: what the strip's USB port gives, with margin: **450** for a
     500 mA port, up to **810** for a 900 mA port. Never more than the port.
   - *LED count*: how many LEDs light, from the start of the strip.
3. Press **Start** (top right card) and use the *Demo* (enable it in *Device*) to check
   that all LEDs light and the colours are right. Then turn the Demo off again.

## 4. Pairing with the TV (Ambilight+hue)

1. On the Ambi page press **Pair (30 s)** (or briefly press the board's button, if
   *Atom button* is set to *Pairing*).
2. Within 30 s on the TV: **Settings → Ambilight → Ambilight+hue → Configure**.
   The TV finds a "Hue bridge" with the name of your WLED device. Choose it.
3. Select the lamps (3 or 5; 3 is enough, 5 gives the corners more colour without
   JointSpace) and give each its **position**: bottom-left, bottom-middle,
   bottom-right (with 5: also the lower left / right side).
4. Turn **Ambilight+hue** on. The Ambi page shows the stream (about 25 frames per
   second) and the strip follows the picture.
5. Optional: **Device → Auto Start**, so the strip follows the TV after every power cut
   without pressing Start.

If the TV ever says it cannot reach the bridge, or the strip stays dark: open
**Diagnosis → Check now**; each problem comes with what to do.

## 5. Lock it down

- WLED → **Config → Security & Updates**: set a **settings PIN** (4 digits) and the
  **OTA lock** with a passphrase. The Ambi page asks for the PIN (🔒 PIN, top right)
  before any change.
- WLED → **Config → WiFi Setup**: change the **AP password** (default `wled1234`).
- WLED → **Config → Sync Interfaces**: leave *Receive UDP realtime* and the WLED
  broadcast *Receive* boxes **off** (nothing else on your network can then drive the
  strip).
- Never forward a router port to the board.
- The Ambi page → **Security** card shows what is still open.

## 6. JointSpace (optional)

The TV's local API gives the colours of its three factory sides; the strip's corners
then match them. It needs a username and password from a JointSpace pairing:

1. TV on. **Bridge & settings → TV connection (JointSpace)** (enter the settings PIN
   first if you set one).
2. Leave *TV IP* empty if the TV has already streamed Ambilight+hue to the board (it
   knows the address then); otherwise type the TV's IP.
3. Press **Pair with the TV**. The TV shows a PIN (usually 4 digits) for about a minute.
4. Type it and press **Send PIN**. "✓ Paired": done; the credentials are stored on the
   board and never shown again.

Already have JointSpace credentials (from another tool)? Type them into the
*TV_USER* / *TV_PASS* fields of the same card instead.

Without JointSpace the strip still works; only the corners are simpler (or use 5
lamps on the TV). Other ways to get the credentials (Home Assistant, a script):
[JOINTSPACE.md](JOINTSPACE.md).

## First install over USB (without WLED's web installer)

```
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 AmbiBottom_vX.YY.bin
```

(This erases nothing else, but a board that never had WLED then starts with empty
WLED settings: continue at step 1.3.)
