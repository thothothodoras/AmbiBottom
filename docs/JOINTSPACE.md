# JointSpace credentials

JointSpace is the Philips TV's local API (HTTPS, port 1926). AmbiBottom uses it for one optional
thing: the colours of the TV's three factory Ambilight sides, so the strip's corners match them.

The strip itself does not need it: the bottom colours come from Ambilight+hue. It can be switched
off on the page (*Use JointSpace*), which also leaves ~45 KB more free memory on the board while
the TV plays.

JointSpace needs a **username and password** that the TV hands out once, in a pairing where it
shows a PIN on screen. There are three ways to get them; use one.

## A. From the Ambi page (easiest)

1. TV on. Ambi page → **Bridge & settings** → **TV connection (JointSpace)**. Enter the WLED
   settings PIN first (🔒 PIN, top right) if you set one.
2. *TV IP*: leave it empty if the TV has already streamed Ambilight+hue to the board (it knows
   the address then); otherwise type the TV's IP (from your router, or the TV: Settings →
   Wireless & networks → Wired or Wi-Fi → View network settings).
3. Press **Pair with the TV**. The TV shows a PIN for about a minute.
4. Type it, **Send PIN**. "✓ Paired": the credentials are stored on the board and are never
   shown again. A wrong PIN can be typed again while the TV still waits.

## B. From Home Assistant (if it already controls the TV)

Home Assistant's *Philips TV* integration paired with the TV the same way and keeps the
credentials. They are not shown in its UI; they are in the file
`config/.storage/core.config_entries` (File editor / Studio Code Server / SSH add-on):

1. Search the file for `"domain": "philips_js"`.
2. In that entry's `"data"`: `"username"` and `"password"`.
3. Type them into **TV connection (JointSpace)** → *TV_USER* / *TV_PASS* → *Save and apply*.

Both Home Assistant and AmbiBottom can use the same credentials at the same time.

## C. With a script on a computer

`tools/jointspace_pair.py` (Python 3, standard library only) does the same pairing from any
computer on the same network:

```
python3 tools/jointspace_pair.py 192.0.2.10
The TV shows a PIN now (for about 60 s).
PIN: 1234

Paired. Type these into the AmbiBottom page (TV connection (JointSpace)):
  username (TV_USER): 0f3c...
  password (TV_PASS): 9a1e...
```

Then type them into the page as in B.3.

## Notes

- **Keep them private.** With them, anyone on your network can control the TV. The board never
  shows them again, never puts them in the log or the Diagnosis report.
- Each pairing makes new credentials; older ones usually keep working. Pairing again from the page
  replaces the ones stored on the board.
- The TV answers on port 1926 only while it is on (or in a "network standby" that keeps Wi-Fi
  awake). The board asks the TV for the side colours only while the TV streams Ambilight+hue.
- Models: Philips Android TVs (about 2016 on) use this API version (6). Older or Titan OS TVs are
  untested.
