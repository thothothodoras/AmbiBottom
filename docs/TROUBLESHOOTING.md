# Troubleshooting

Where to look first: the monitor page (`http://<bridge>:8098/`), the log
(`<state dir>/ambi_bridge.log`, e.g. `/var/lib/ambi-bridge-cpp/ambi_bridge.log`),
and on the ESP32 WLED's Info page and `http://<esp>/ambibottom/status`.

The explanations below come from two sources, marked: **[seen]** = observed on
the real TV (65OLED707/12, firmware TPM211EA_R.101.001.005.115);
**[TV code]** = read from the TV's own software (`docs/TV-RE-FINDINGS.md`),
not measured.

---

## 1. The TV's Hue menu is grey

### "Immersive experience" is grey, the rest works

**This is normal. Nothing is broken.** The TV offers "Immersive experience"
only for bridges *without* Entertainment (streaming). With this bridge the TV
pairs for streaming, so it greys the item out — also while the stream runs
perfectly. [seen: the TV's own help text says so; TV code: the menu item is
enabled only when the TV has no stream key stored]

Do not try to "fix" it: the only way to make it selectable is a bridge that
does not stream, and then you get the slow non-stream mode.

### The whole Ambilight+hue menu is grey

| Cause | What to do |
|---|---|
| Ambilight style is "Off" | Choose a style (e.g. Follow video). [TV code] |
| The TV thinks Ambilight+hue is not configured | Run **Configure** in the Ambilight+hue menu again. |
| You were in the TV's *factory menu* and moved the "AMB Type" selector | That selector also overwrites the "Ambilight+hue configured" flag [TV code]. Leave the factory menu alone; run Configure again, or let the TV start a stream once (it sets the flag back). |

---

## 2. "LOST ITS PAIRING" in the log / "The TV wants to pair" on the page

Log line:

```
[rest] 192.168.x.x uses an UNKNOWN 32-hex username (xxxxxxxx...): the TV has LOST ITS PAIRING with this bridge
```

**What it means.** The TV no longer has the username this bridge gave it.
It then uses a built-in fallback name (derived from the TV's own production
code) that the bridge never issued. The bridge answers "unauthorized user",
as a real Hue bridge does. The TV cannot stream until it pairs again.
[seen 2026-11-01: after a TV restart the TV came with such a name; TV code:
where the fallback comes from. Why the TV sometimes loses the stored name is
not known.]

**What to do.**

1. On the bridge: press **Pair** on the monitor page (or `p` in the terminal).
   On the ESP32: press the button on the AtomS3 (the LED blinks blue for 30 s).
2. On the TV, within 30 s: Settings → Ambilight → Ambilight+hue → **Configure**,
   choose the bridge, pick the lamps, give each its position.
3. The log should show `pairing ok` and then `stream.active=true`.

If the TV does not find the bridge in step 2: is the bridge running, on the
same network as the TV, and reachable on **port 80**? The TV only ever talks
to port 80.

**Positions (Angle) in the TV's wizard.** The TV decides which part of the
picture a lamp shows from the *Angle* you give it in the wizard, not from
anything the bridge says. For the bottom: the three bottom positions are
bottom-left, bottom-middle, bottom-right; with five lamps the outer two are
the lower left and lower right *side* positions. [seen: measured with test
patterns 2026-11-06; TV code]

---

## 3. "The TV locked itself to LightGroup after a DTLS failure — restart the TV"

Banner on the monitor page, `TV LOCKED` in the terminal, a red status LED on
the ESP32, `tv_dtls_lock` in the state JSON. Log line:

```
[main] no DTLS handshake within 3 s of stream.active=true: the TV has locked itself to LightGroup (no stream) - it needs a TV RESTART to try again
```

**What it means.** The TV asked for the stream (`stream.active=true`) and
the encrypted connection (DTLS) was not up within 3 seconds, or its
handshake failed. From then on the TV stops trying to stream: it falls back
to its slow non-stream mode ("LightGroup") and stays there until its Hue
service restarts. Turning Ambilight+hue off and on is not enough. [TV code:
the 3 s, and that the failure is remembered until the process restarts. Not
measured on the TV — the banner is built on that reading.]

**What to do.**

1. Find out why the handshake failed, or it will happen again. In the log,
   around the time of the banner:
   - `Unknown identity` / `Verification of the message MAC failed` → the TV
     uses a key the bridge does not have: it is the lost pairing of section 2.
     Pair again first.
   - nothing at all from `[dtls]` → the stream port did not reach the bridge:
     UDP **2100** must be open from the TV to the bridge (firewall, Docker port
     mapping, another program on the port).
   - `DTLS handshake 3.x s after stream.active=true ... TOO LATE` → the bridge
     was too slow (overloaded machine).
2. **Restart the TV**: unplug it for a few seconds (standby is not a restart).
3. After it boots the log should show, in this order: `asked groups/N ->
   answered Entertainment`, `stream.active=true`, `DTLS handshake 0.0x s after
   stream.active=true`. The banner clears itself with the first good handshake.

If the banner shows but the strip follows the TV anyway, the reading of the
TV's code was wrong for your TV: please report it.

---

## 4. Other things that look similar

### "The TV streams from an OLD session" (magenta LED on the ESP32)

The bridge was restarted (or the ESP rebooted) while the TV was streaming. The
TV keeps sending with the keys of the session that is gone. **On the TV turn
Ambilight+hue off and on** (or change the Ambilight style and back). No new
pairing, no TV restart. The Linux bridge avoids this for its own clean
restarts by keeping the session; the ESP32 cannot. [seen]

### The strip is lit but the colours are wrong / white looks blue

Not a connection problem: the strip is not colour-calibrated yet. See
`docs/CALIBRATION.md`.

### The strip stays dim on bright pictures

The current limit is working (default 450 mA; full white on 43 LEDs would want
about 2.6 A). Raise it only if your supply really delivers more:
`docs/HARDWARE.md`, "Power".

### The TV creates a new group every time / many groups in `groups.json`

Fixed in v0.31. The first start of v0.31 or later tidies an older store
(the old file is kept as `groups.json.pre-v031`).

### After a bridge restart the page shows "live" but the TV shows nothing new

That is section 4, "old session", on a build before v0.29.

---

## Still stuck — what to collect

- the log from the start of the bridge until the problem (it never contains
  stream keys; since v0.32 usernames are cut to 8 characters - a log from
  v0.31 or older has them in full: remove them before you post it);
- the monitor page's state: `curl http://<bridge>:8098/api/state`;
- TV model and firmware version; whether the TV was restarted, and how;
- what the TV's Ambilight+hue menu shows (a photo is fine).
