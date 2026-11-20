#!/usr/bin/env python3
"""Checks the datagrams tools/ddp_listener.py recorded against the DDP format a
stock WLED accepts (wled00/e131.cpp handleDDPPacket) and against what the
bridge says it sent. Prints [ok]/[FAIL] lines; exit code = number of failures.

usage: python3 tools/ddp_check.py packets.json --leds 43 [--state state.json] [--none]
  --state: a saved /api/state of the bridge taken while a STILL picture was
           streamed: the last frame on the wire must equal output.leds.
  --none : pass only if nothing at all was received (output switched off).
"""
import argparse, json, sys

ap = argparse.ArgumentParser()
ap.add_argument("packets")
ap.add_argument("--leds", type=int, default=43)
ap.add_argument("--state")
ap.add_argument("--min-fps", type=float, default=15.0)
ap.add_argument("--budget-ma", type=float)
ap.add_argument("--none", action="store_true")
a = ap.parse_args()

fails = 0
def check(name, cond, extra=""):
    global fails
    print(("[ok]   " if cond else "[FAIL] ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails += 1

pk = [dict(p, raw=bytes.fromhex(p["hex"])) for p in json.load(open(a.packets))]
if a.none:
    check("no datagram at all without --ddp-target", len(pk) == 0, f"{len(pk)} received")
    sys.exit(fails)

check("datagrams arrived", len(pk) > 0, f"{len(pk)}")
if not pk:
    sys.exit(fails or 1)
raws = [p["raw"] for p in pk]
want_len = 10 + 3 * a.leds
check(f"every datagram is {want_len} bytes (10-byte header + 3 x {a.leds} LEDs, one packet per frame)",
      all(len(r) == want_len for r in raws), f"sizes seen: {sorted(set(len(r) for r in raws))}")
check("flags: DDP version 1 with PUSH (0x41), no timecode, no query/reply bits", all(r[0] == 0x41 for r in raws),
      f"seen: {sorted(set(hex(r[0]) for r in raws))}")
seqs = [r[1] for r in raws]
check("sequence numbers 1..15 (never 0), each one the previous + 1, wrapping 15 -> 1",
      all(1 <= s <= 15 for s in seqs) and all(seqs[i + 1] == (1 if seqs[i] == 15 else seqs[i] + 1) for i in range(len(seqs) - 1)),
      f"first ten: {seqs[:10]}")
check("data type 0x0B (RGB, 8 bits per channel) and destination id 1 (the default output)",
      all(r[2] == 0x0B and r[3] == 0x01 for r in raws))
check("offset 0 and length = 3 x LEDs in the header",
      all(int.from_bytes(r[4:8], "big") == 0 and int.from_bytes(r[8:10], "big") == 3 * a.leds for r in raws))
span = pk[-1]["t"] - pk[0]["t"]
fps = (len(pk) - 1) / span if span > 0 else 0.0
check(f"rate >= {a.min_fps:.0f} frames/s while the stream runs", fps >= a.min_fps, f"{fps:.1f} frames/s over {span:.1f} s")
check("sent from this machine", all(p["from"] == "127.0.0.1" for p in pk))
last = [list(raws[-1][10 + 3 * i:13 + 3 * i]) for i in range(a.leds)]
check("the picture is not black", any(sum(c) for c in last), f"LED 0 = {last[0]}, middle = {last[a.leds // 2]}, last = {last[-1]}")
if a.state:
    st = json.load(open(a.state))
    out = st.get("output", {})
    leds = out.get("leds") or []
    got = [[c.get("r"), c.get("g"), c.get("b")] if isinstance(c, dict) else list(c)[:3] for c in leds]
    still = [list(r[10:]) for r in raws[len(raws) // 2:]]
    stable = all(x == still[0] for x in still)
    check("a still picture gives identical frames (second half of the run)", stable)
    mid = [list(raws[len(raws) // 2][10 + 3 * i:13 + 3 * i]) for i in range(a.leds)]
    check("the bytes on the wire are exactly the monitor's output.leds (linear LED values)", got == mid,
          f"monitor LED 0 = {got[0] if got else None}, wire LED 0 = {mid[0]}")
    if a.budget_ma is not None:
        ma = out.get("estimated_ma", 1e9)
        check(f"estimated current within the {a.budget_ma:.0f} mA budget", ma <= a.budget_ma + 0.01, f"{ma:.1f} mA")
sys.exit(fails)
