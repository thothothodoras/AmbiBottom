import glob, json, os, re, sys
D = os.environ.get("JSON_DIFF_DIR", "/tmp/rest_diff")  # v0.36: tools/esp_hue_diff.sh points it elsewhere
LA, LB = os.environ.get("JSON_DIFF_LABELS", "OLD,CPP").split(",")
def load(path):
    t = open(path).read()
    t = re.sub(r'[0-9a-f]{32}', 'USER', t)              # usernames
    t = re.sub(r'[0-9A-F]{32}', 'KEY', t)               # clientkeys
    t = re.sub(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d', 'TS', t)
    t = re.sub(r'(?i)b2[:]?80[:]?d[9a]', 'MAC3', t)     # v0.31: old bridge runs with MAC ..:d9
    # v0.31: group ids differ on purpose (C++ starts at 1, reuses groups)
    t = re.sub(r'/groups/\d+', '/groups/N', t)
    t = re.sub(r'"id": ?"\d+"', '"id":"N"', t)
    t = re.sub(r'Group \d+', 'Group N', t)
    try: return json.loads(t)
    except Exception: return t
def walk(a, b, path=""):
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in b: print(f"   only {LA:4s} {path}/{k} = {json.dumps(a[k])[:150]}")
            elif k not in a: print(f"   only {LB:4s} {path}/{k} = {json.dumps(b[k])[:150]}")
            else: walk(a[k], b[k], f"{path}/{k}")
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)): walk(x, y, f"{path}[{i}]")
    elif a != b:
        print(f"   DIFF      {path}: {LA.lower()}={json.dumps(a)[:120]}  {LB.lower()}={json.dumps(b)[:120]}")
for o in sorted(glob.glob(f"{D}/old/*.out")):
    n = os.path.basename(o)[:2]
    c = glob.glob(f"{D}/cpp/{n}_*.out")[0]
    a, b = load(o), load(c)
    name = re.sub(r'_groups_\d+', '_groups_N', re.sub(r'[0-9a-f]{32}', '<user>', os.path.basename(o)[3:-4]))
    print(f"== {n} {name}" + ("   (identical)" if a == b else ""))
    if a != b: walk(a, b)
