# Hardware — parts, wiring, power

What the reference build uses. Marked **[checked]** = verified from a
schematic, a datasheet or the device itself; **[measured]** = measured on the
reference parts with a USB tester (10 mA steps); **[spec]** = the vendor's
number, not measured here; **[not measured]** = an assumption you must check
on your own setup.

## Parts

| Part | Used here | Notes |
|---|---|---|
| Controller | M5Stack **AtomS3 Lite** | **[checked on the device, 2026-11-17, esptool-js]** ESP32-S3 (QFN56) rev v0.2, **8 MB embedded flash (XMC), no PSRAM**, native USB (VID 0x303a, PID 0x1001). USB-C. Data out on **GPIO 38** (bottom socket, the 4-pin side: G39, G38, 5V, GND) [works, 2026-11-17]. Top button = **GPIO 41** [works as WLED Button 0]. On-board RGB LED = GPIO35, Grove port = G1/G2 [M5Stack's pinout, not checked]. |
| Firmware | **WLED 16.0.1**, stock, from install.wled.me (Advanced: **Plain, 8MB, Memory Standard, Flash mode Default**) [checked: runs, free heap **204 KB** without any usermod] | Mode B: the Linux bridge sends the colours over DDP. Our usermod build (Mode A, `firmware/`) is for WLED 0.15 and has never run on the device. |
| Level shifter (**optional** since 2026-11-17) | Adafruit **Pixel Shifter #6066** | The strip works **without** it: 3.3 V data straight from GPIO 38 over a short wire gave correct colours and a Rainbow without flicker [checked by eye, 10 LEDs, 2026-11-17]. Keep it for a longer data wire or if flicker / wrong colours ever show. What it is: turns 3.3 V data into 5 V; inputs `G, V, DAT, CLK`, outputs `C5, !D5, D5, G`; own 5 V charge pump; **already 100 Ω in series on every output**; **no power passes through it** (`C5` is a clock output); one WS2812B of its own on the data line [checked against Adafruit's Eagle schematic]. |
| LED strip | **BTF-LIGHTING WS2812B "ECO"**, 30 LED/m, 5 V, black PCB, IP30 | 43 LEDs ≈ 1.43 m for a 65" TV (≈ 39 if mounted like the TV's top strip, 8 cm in from each end). Wires [checked against the vendor's PDF]: **red = +5 V, green = data, white = GND**; the plug also has an extra red / white pair for power injection ("voltage compensation"). **≈ 44 mA per LED at full white [measured, see Power]**; the vendor says 9 W/m = 60 mA. Colour order **GRB** [checked]. |
| Fuse | Resettable fuse (PTC, "polyfuse"), **1.1 A hold** | In series in the strip's +5 V, as close to the USB plug as possible. See "Power". |
| Strip power | its **own** USB supply (a TV USB port or a charger) | See "Power". |
| ESP power | another USB port (TV or any 5 V USB supply) | The AtomS3 itself: **0-30 mA** on the tester [measured; below the tester's resolution, so not a real figure; WLED budgets 120 mA for it]. |
| Capacitor (**needed**, v0.33) | electrolytic 470–1000 µF, 6.3 V or more | Across +5 V / GND at the start of the strip, **stripe (−) to GND**. Not only for the moment power is applied: see "Average current and PWM" under Power. |
| Mounting (planned) | M5Stack **AtomIC Proto Kit** | Its silk-screen labels are for the **old** ATOM. On the AtomS3, by position [**not checked - confirm with a multimeter before soldering**]: Tx22 = G5, Rx19 = G6, Neo23 = G7, Adc33 = G8, SDA = G38, SCL = G39. The G / V / A / B row: G = GND, V = 5 V (probably already connected), A / B free pads. Alternative: a Grove cable. |

## Wiring

**Permanent setup, decided 2026-11-17: WITH the Pixel Shifter, data on G38** (the user's
friend solders it on 2026-11-18; drawing for him: `docs/wiring.html`, published as a page).
Atom 3V3 → 6066 V, Atom GND → 6066 G (INPUT), Atom G38 → 6066 DAT, 6066 CLK bridged to
6066 G (CLK is unused; tied so it does not float. The board does NOT tie it itself: in the Adafruit schematic CLK_3V goes straight to input 2A of the 74HCT2G34, no pull-down. Adafruit's own NeoPixel example leaves it open), 6066 D5 → strip DIN, 6066 G (OUTPUT) →
strip GND (white of the plug), USB 2 +5V → F1 polyfuse 1.1 A → strip +5V (red of the power
pair), USB 2 GND → strip GND (white of the power pair), C1 470-1000 µF across +5V / GND at the
strip start (stripe to GND). Common GND point (TB1 on the friend's drawing) = the strip start.
Electrically the same as a star point at a terminal block; what matters: the strip current
returns straight to USB 2, and the data ground meets the strip ground next to DIN.

Without the shifter (works on the bench, 3.3 V data, short wire):

```
                       AtomS3 Lite                                    LED strip (WS2812B, BTF)
   USB (ESP)  ───────► USB-C
                       G38 (bottom socket) ─── short wire ──────────► DIN   (green)
                       GND (bottom socket) ─────────────────────────► GND   (white) ──┐
                       5V pin: NOT connected to the strip                              │
                                                                                       │
   USB (strip) +5V ──►[ polyfuse 1.1 A ]──┬────────────────────────► +5V   (red)      │
                                          │+                                           │
                                       capacitor 470-1000 µF                           │
                                          │− (stripe)                                  │
   USB (strip) GND ───────────────────────┴─────────────────────────────────────────►─┘
```

With the Pixel Shifter: as in the permanent setup above; `C5`, `!D5` not connected.

**Bench test of 2026-11-17 (not the permanent setup):** 10 LEDs fed from the
Atom's own 5 V pin, data on G38, WLED's ABL at 450 mA. Fine for a few LEDs;
the full strip must not be fed through the Atom.

Rules:

1. **Common ground.** The strip's GND and the ESP's GND are connected (the
   wire next to the data wire). With two USB ports of the same TV they
   already share ground inside the TV; the wire makes it explicit.
2. **Never join the two +5 V lines.** The ESP's 5 V (its USB) and the strip's
   5 V (the other USB, through the fuse) stay separate. Only ground and the
   data line go across. In the permanent setup the Atom's 5V pin stays
   unconnected.
3. **The strip's power does not go through the ESP (or the shifter).** +5 V
   and GND go straight from the USB plug (through the fuse) to the strip; the
   plug's extra red / white pair can carry it.
4. **Data goes in at DIN**, the end of the strip where the arrows point away.
   The strip has solder joints every 0.5 m (after LED 15 and 30 on a 43-LED
   length): do not bend it there.
5. Keep the data wire short (a few centimetres; with the shifter up to ~30 cm).

## Flashing WLED from Windows (2026-11-17)

- **Chrome or Edge** (Web Serial), install.wled.me, Advanced: Plain, 8MB,
  Memory Standard, Flash mode Default.
- On a **USB 3** port the Atom did not appear; on a **USB 2.0** port it did.
- **Download mode:** hold the side button ~2 s until the green LED lights.
- **Close the serial port before unplugging** (Disconnect, or close the tab).
  Unplugging with the port open hung Windows and needed a restart.
- After flashing: WLED-AP (password `wled1234`) → home Wi-Fi. The reference
  Atom got 192.0.2.20 (to be fixed by a DHCP reservation; `--wled-ip`
  takes an IP). Home Assistant found it by itself.
- WLED **Button 0 = GPIO 41**. Careful: holding it opens the AP (~6 s) or
  does a **factory reset** (~12 s), which also clears the ABL settings.

## Power — read this before plugging in

**43 LEDs at full white want about 1.9 A [measured: 43 × 44 mA]. A TV's USB
port gives 0.5 A or 0.9 A.** The strip therefore runs behind a current
limit, always:

| Layer | Where | Speed | Value |
|---|---|---|---|
| 1. The pipeline's limit | bridge (`current_budget_ma`) or usermod (`budgetMa`) | every frame | default **450 mA**; never above the build's ceiling **900 mA** (`AMBI_MAX_BUDGET_MA`) |
| 2. WLED's limiter (ABL) | WLED → LED Preferences → "Maximum current" | every frame | see "WLED's limiter: which number" below (it is NOT "a little above layer 1") |
| 3. Polyfuse 1.1 A | hardware, in the strip's +5 V | seconds | protects against a fault or a bug that lasts; not against short peaks |
| 4. The TV's own port protection | TV | unknown | the TV may switch the port off |

### Measured on the reference strip (2026-11-17)

USB tester, 10 mA steps, 5.00 V; WLED brightness at maximum, **ABL off**,
full white (255, 255, 255):

| LEDs lit | Current |
|---|---|
| 5 | 0.20 A |
| 10 | 0.42 A |

→ **≈ 44 mA per LED** (range 40–48 from the tester's ±10 mA on each
reading); the Atom itself 0–30 mA (below the tester's resolution). The
bridge's default `ma_per_led_full_white` is **45** since v0.34 (was 60 =
the vendor's 9 W/m); the model adds 1 mA idle per LED, so it counts 46 per
LED at white, a little above the measurement. Still settable (1–200): a
different strip must be measured (README "Measuring mA per LED").
Not measured yet: red / green / blue separately, the idle current of a dark
strip, the TV port's real current and its standby behaviour.

### WLED's limiter: which number (WLED 16.0.1 and 0.15, read from the code)

WLED takes **`MA_FOR_ESP` = 120 mA** (ESP32; 80 on an ESP8266) off the
"Maximum current" you enter before it compares ("~100 mA" seen on the
device's page is this 120), and counts 1 mA per LED idle
(`bus_manager.cpp`: `BusDigital::estimateCurrent`, `BusManager::applyABL`;
same in 0.15.5 and 16.0.1). Its estimate is
`(r + g + b) / 765 × LED current + 1 mA` per LED, after its brightness:
**the same model as the bridge** (checked: `tools/v034_test.sh` B, within
2 mA on the same frames). So, with "LED current" (mA/LED, Custom) set to the
same number as the bridge (**45**):

| Setup | Enter in "Maximum current" | Why |
|---|---|---|
| **Mode B, strip on its own USB** (permanent) | **budget + 120** (570 for 450; 920 for 800) | WLED subtracts 120 mA for an ESP that is not on the strip's supply; with +120 WLED's limit equals ours and never dims the bridge's frames |
| **Bench, strip fed from the Atom's 5 V** | **= the supply's limit** (450 on 2026-11-17) | the ESP really is on the same supply, so the 120 is right |
| **Mode A**, our usermod build (`-D MA_FOR_ESP=0`) | **= the budget** | nothing is subtracted |

If WLED's number is lower than the rule, nothing unsafe happens: WLED dims
the whole strip evenly on top of the bridge's limit (darker, and the
limiter's peaks are lost). `python3 tools/wled_check.py <ip>` reads the
WLED's settings (GET only) and says which rule they match.

**What WLED 16.0.1 does with DDP data (read from the code; ABL and
brightness also checked on the device 2026-11-17, see the table at the end):**

- **ABL: applies.** DDP pixels go into WLED's frame buffer and out through
  the same `BusManager::show()` → `applyABL()` as WLED's own effects.
- **Transitions: none.** In realtime mode WLED does not blend segments
  (`WS2812FX::show`), so each DDP frame is shown as it arrives; the
  "Transition" time only affects WLED's own changes.
- **Brightness: applies unless "Force max brightness" is on** (Sync
  Interfaces → Realtime). Off (the default) = WLED's brightness slider
  scales every DDP colour, and the bridge's current estimate is then too
  high (safe but wrong). Turn it on.
- **Gamma: off by default** ("Disable realtime gamma correction" on): the
  bridge already sends linear values. Leave it on.

**The pipeline's limit only covers the frames the pipeline sends.** WLED
lights the strip by itself at power-on (its boot preset), from its own page,
presets and effects, from any other DDP / E1.31 sender, and 2.5 s after the
stream ends or the bridge dies. In all of those the ABL is the ONLY limit:
set it BEFORE the full strip is connected, and untick "Turn LEDs on after
power up". After a factory reset (button held ~12 s) WLED's ABL is back at
its defaults: set it again before reconnecting the strip.

### Average current and PWM (v0.33 note - NOT measured)

Every number above is an **average** current. A WS2812B dims by switching its
LEDs fully on and off (PWM): while a channel is "on" it draws its full
current (about a third of the mA-per-LED figure), whatever the brightness. If
many LEDs are on at the same instant, the strip draws far more than the
average for a fraction of a millisecond - up to the full ~1.9 A in the worst
case. The capacitor at the start of the strip is what evens this out for the
USB port; that is why it is in the parts list as needed, not optional. How
large these peaks really are on this strip, and what the TV's port does with
them, has not been measured (it needs an oscilloscope on the +5 V line).

### What the limit does at the start and after a pause (v0.33)

Until v0.32 the first frame of a stream, and the first frame after a pause,
went from dark to the full budget in one step (sandbox: 0 -> 449.8 mA). Now
the strip ramps over `rise_ms` (default 120 ms). On Stop and Exit the bridge
sends black frames, so the last picture does not stay lit for WLED's 2.5 s.

About the numbers:

- **450 mA is a default, not a measurement.** It was chosen to stay under a
  500 mA port. This TV (65OLED707) labels one port 900 mA and the others
  500 mA [read off the TV by the owner, not measured]. If your strip port
  really gives 900 mA you can raise the limit (to about 800); if you do not
  know, leave 450.
- **45 mA per LED is measured on the reference strip** (above). The limit is
  computed from it, so if your strip draws more, it draws more than the limit
  says. Never enter less than the real value.
- **The three colours are assumed to draw the same.** The estimate is
  `idle + (r·ma_scale_r + g·ma_scale_g + b·ma_scale_b) / (3·255) × mA per LED`
  with `ma_scale_r/g/b` = 1 / 1 / 1 [not measured]. To measure: 10 LEDs at
  full red, then green, then blue (WLED's own page, ABL off for the moment,
  strip on a supply that can give it); for each, (mA above dark) ÷ 10 = mA of
  that channel; scale = that ÷ (white mA per LED ÷ 3). Never enter less than
  the real value: the limit would let more through than it says.
- **1 mA idle per LED** is the usual datasheet figure [not measured here].
- **There is no lower limit for "mA per LED"** (it accepts 1..200): whoever
  can change the settings can therefore weaken the limit by entering a low
  figure; the build-time ceiling only caps the budget itself. The polyfuse
  and WLED's ABL do not depend on it.
- **Polyfuse 1.1 A hold**: "hold" is the current it carries forever; it trips
  at roughly twice that, after seconds. With a 450 mA limit it never comes
  near tripping in normal use — it is there for the day the software is
  wrong. It drops about 0.1–0.2 V at full current [typical; not measured].
- A black picture is not a dark strip for free: every WS2812B draws about
  1 mA idle (43 mA for the strip), and the TV does not send pure black for a
  black picture (measured 2026-11-06: about 200 mA on black with the old
  800 mA settings).

## What was and was not verified

| | |
|---|---|
| AtomS3 Lite: chip, 8 MB flash, no PSRAM, native USB | **checked on the device** (esptool-js, 2026-11-17) |
| GPIO 38 data, GPIO 41 button | **work** (WLED, 2026-11-17); GPIO35 LED, Grove pins: M5Stack's documentation only |
| WLED 16.0.1 Plain 8MB on the Atom | **runs**, free heap 204 KB, Wi-Fi, found by Home Assistant |
| Strip without the Pixel Shifter (3.3 V data, short wire) | **works** by eye: colours right (GRB), Rainbow without flicker, 10 LEDs |
| Pixel Shifter pinout, 100 Ω outputs, no power pass-through | checked against the schematic (not in use now) |
| Strip wire colours | vendor's PDF |
| mA per LED at full white | **measured: ≈ 44 (40–48)**, 2026-11-17 |
| Current of red / green / blue separately (`ma_scale_r/g/b`) | assumed equal, not measured |
| Idle current per LED (1 mA) | datasheet figure, not measured |
| Peak (PWM) current vs. the average the limit works with | not measured |
| WLED's 120 mA for the ESP, its current model | read from WLED 16.0.1's source; matches the "~100 mA" seen on the device |
| WLED's brightness on DDP data | **checked on the device 2026-11-17**: "Force max brightness" off, WLED brightness 128, `wled_check.py --ddp-test 1`: WLED's estimate 32 mA for the LEDs (= 10 idle + 45 × 128/255 for the one white LED, not 45 + 10), tester ≈ 0.02 A = one LED at half power. So WLED's slider DOES scale DDP colours: turn "Force max brightness" on |
| WLED's ABL on DDP data | **checked on the device 2026-11-17**: 10 LEDs full white over DDP (`wled_check.py --ddp-test 10`), ABL 450 / 45 mA, strip on the Atom's 5 V: tester **0.29-0.30 A** (Atom included) instead of 0.42 A without ABL. WLED's limiter dims to 330 / 460 = 72 %, and 0.42 A × 0.72 = 0.30 A: matches. So WLED's ABL acts on the bridge's frames, and its model (45 mA + 1 idle) is a little above the real draw, i.e. on the safe side |
| No transitions on DDP data | **checked by eye 2026-11-17**: at the end of `--ddp-test 10` the LEDs went dark at once (no fade), as WLED 16.0.1's source says. (`/json/live` answers 501 on the release build, `WLED_ENABLE_JSONLIVE` not set) |
| AtomIC Proto Kit pin mapping | by position only, **not checked** |
| The 900 mA ceiling | the label of the TV's largest USB port, not measured |
| What the TV's USB ports really deliver, and in standby | the label on the TV, not measured |
| Polyfuse voltage drop and trip behaviour | typical values, not measured |
| Mode B end to end (TV → bridge v0.34 → DDP → WLED 16.0.1 → strip) | **works, 2026-11-17 16:18**: Hue and JointSpace live, DDP 24.5 frames/s, 0 errors, 10 LEDs on the bench (bridge `led_count` 10, budget 330 while the strip hangs on the Atom's 5 V); all LEDs lit as expected |
| The whole thing lit under the TV | **not yet** (10 LEDs on the bench only) |
