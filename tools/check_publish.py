#!/usr/bin/env python3
"""The checks behind tools/check_publish.sh. File names on stdin, repo root as argv[1]."""
import os, re, sys

root = sys.argv[1]
files = [l.strip() for l in sys.stdin if l.strip()]
problems = []

def add(kind, path, detail=""):
    problems.append((kind, path, detail))

# 1) file names that are state / keys / the TV's software, whatever .gitignore says
BAD_NAME = re.compile(r"(^|/)(pairing[^/]*\.json|groups\.json[^/]*|jointspace_credentials\.json|[^/]*credentials[^/]*\.json|login\.json|"
                      r"dtls_session\.bin|[^/]*\.nvs|[^/]*\.pem|[^/]*\.key|adbkey[^/]*|id_rsa[^/]*|\.env[^/]*)$", re.I)
TV_NAME = re.compile(r"\.(apk|dex|odex|vdex|smali|jar)$|(^|/)tv-re[^/]*/|(^|/)tv-re-apks|(^|/)lib[^/]*\.so$")
# 2) content
PRIVATE_KEY = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")
HEX32 = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{32}(?![0-9A-Fa-f])")
# 32-hex values that are test dummies or public constants, not somebody's username / stream key
DUMMY = re.compile(r"^(0123456789abcdef0123456789abcdef|00112233445566778899aabbccddeeff|a{30}\d\d|f{32}|0{32}|"
                   r"d00e962cbb17ac85a1ba9a8e9cbe3328)$", re.I)
# a real value next to these words is a leaked secret; the words alone (code, docs) are fine
SECRET_ASSIGN = re.compile(r"""(clientkey|client_key|username|password|passwd|tv[-_]pass|tv[-_]user|digest|psk|secret|token)["']?\s*[:=]\s*["']([^"'\s<>{}$]{8,})["']""", re.I)
DECOMPILED = re.compile(r"^\s*(package|import)\s+(org\.droidtv|com\.mediatek|vendor\.tpv)\b|/\* JADX|JADX (INFO|WARN)|loaded from: classes\d*\.dex|^\s*\.method |invoke-(virtual|static|direct) ", re.M)
# Looked at by hand: made-up values that a test types into a sandbox bridge it
# starts itself on a test port (never a real login).
KNOWN_TEST_VALUES = {
    ("tools/v028_test.sh", "password"),  # the monitor-login test: sets, uses and removes its own password
}
TEXT_EXT = (".cpp", ".hpp", ".h", ".c", ".py", ".sh", ".md", ".txt", ".json", ".ini", ".cmake", ".service", ".csv", ".html", ".js", ".yml", ".yaml", "")

for rel in files:
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        continue
    if BAD_NAME.search(rel):
        add("state or key file", rel)
    if TV_NAME.search(rel):
        add("TV software file", rel)
    size = os.path.getsize(path)
    if size > 5 * 1024 * 1024:
        add("large file (> 5 MB)", rel, f"{size // 1024} KB")
    ext = os.path.splitext(rel)[1].lower()
    if ext not in TEXT_EXT or size > 2 * 1024 * 1024:
        continue
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        continue
    if rel in ("tools/check_publish.py",):
        continue  # this file names the patterns it looks for
    if PRIVATE_KEY.search(text):
        add("private key", rel)
    for m in HEX32.finditer(text):
        v = m.group(0)
        if DUMMY.match(v):
            continue
        line = text.count("\n", 0, m.start()) + 1
        add("32 hex digits (a Hue username or stream key?)", rel, f"line {line}: {v[:6]}...{v[-2:]}")
    for m in SECRET_ASSIGN.finditer(text):
        v = m.group(2)
        if (rel, m.group(1).lower()) in KNOWN_TEST_VALUES:
            continue
        if DUMMY.match(v) or re.match(r"^(\.\.\.|x+|\*+|<.*>|your.*|example.*|changeme|USER|KEY|none|null|true|false)$", v, re.I):
            continue
        if re.search(r"[%$\\(){}\[\]]|std::|\.c_str|argv|getenv", v):
            continue  # code, not a value
        line = text.count("\n", 0, m.start()) + 1
        add("a value next to a secret's name", rel, f"line {line}: {m.group(1)} = {v[:4]}...")
    m = DECOMPILED.search(text)
    if m:
        line = text.count("\n", 0, m.start()) + 1
        add("looks like decompiled TV code", rel, f"line {line}")

if not problems:
    print("OK: no secrets, no state, no TV software, no decompiled code found")
    sys.exit(0)
for kind, rel, detail in problems:
    print(f"[FOUND] {kind}: {rel}" + (f"  ({detail})" if detail else ""))
print(f"{len(problems)} finding(s): fix or explain each one before publishing")
sys.exit(1)
