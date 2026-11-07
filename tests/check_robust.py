#!/usr/bin/env python3
"""Hostile inputs: every command must fail cleanly (an exit status, a
message), never crash or trip a sanitizer.

Usage: check_robust.py MMDBIO WORKDIR

Builds a small database, then feeds the readers corrupted and truncated
copies of it, and feeds import broken gzip, CSV and JSON. With an ASan/UBSan
build of mmdbio, sanitizer reports count as failures too.
"""
import gzip
import os
import random
import subprocess
import sys

MMDBIO, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
rng = random.Random(3)
failures = 0
runs = 0


def run(args, label):
    global failures, runs
    runs += 1
    try:
        p = subprocess.run([MMDBIO, *args], capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        failures += 1
        print(f"FAIL  {label}: timed out")
        return None
    err = p.stderr.decode("utf-8", "replace")
    if p.returncode < 0 or "AddressSanitizer" in err or "runtime error:" in err or "LeakSanitizer" in err:
        failures += 1
        print(f"FAIL  {label}: exit {p.returncode}\n{err[-2000:]}")
    return p


def path(name):
    return os.path.join(WORK, name)


# a database with a bit of everything
with open(path("base.json"), "w") as f:
    f.write('{"1.2.3.0/24": {"s": "x", "n": 5, "d": 1.5, "b": true, "a": [1, "two"], "m": {"k": "v"}},'
            ' "2001:db8::/32": {"s": "y"}, "9.9.9.9": {"big": 340282366920938463463374607431768211455}}')
p = run(["import", "--in", path("base.json"), "--out", path("base.mmdb"), "--alias-6to4", "-q"], "build base")
data = open(path("base.mmdb"), "rb").read()

readers = [
    ["read", "--db", "DB", "--ip", "1.2.3.4"],
    ["read", "--db", "DB", "--range", "1.2.3.0/30"],
    ["metadata", "--db", "DB"],
    ["stats", "--db", "DB", "--json"],
    ["inspect", "--db", "DB", "--sample-ip", "1.2.3.4"],
    ["verify", "--db", "DB"],
    ["export", "--db", "DB", "--out", "-"],
    ["export", "--db", "DB", "--where", "s=x", "--format", "cidr", "--out", "-"],
    ["diff", "--old", "DB", "--new", path("base.mmdb"), "--json"],
    # the commands and formats beyond the Go mmdbio
    ["read", "1.2.3.4", "1.2.3.0/30", "2001:db8::1", "DB", "-f", "csv", "--flatten"],
    ["read", "9.9.9.9", "DB", "-f", "jsonl"],
    ["export", "DB", "-f", "csv"],
    ["export", "DB", "-f", "tsv", "--flatten"],
    ["export", "DB", "-f", "jsonl"],
    ["metadata", "DB", "--json", "--data-types"],
    ["metadata", "DB", "--layout", "--data-types"],
    ["diff", "DB", path("base.mmdb"), "--subnets", "--records", "--json"],
    ["shrink", "DB", path("bad.shrunk.mmdb"), "--overwrite", "--lookups", "300", "--bench=false", "--full-check"],
    ["shrink", "DB", "--dry-run", "--record-size", "24"],
]

cases = []
for i in range(60):
    b = bytearray(data)
    for _ in range(rng.choice([1, 2, 8, 40])):
        b[rng.randrange(len(b))] = rng.randrange(256)
    cases.append((f"flipped bytes #{i}", bytes(b)))
for cut in (0, 1, 10, 100, len(data) // 2, len(data) - 20, len(data) - 1):
    cases.append((f"truncated to {cut} bytes", data[:cut]))
cases.append(("random bytes", bytes(rng.randrange(256) for _ in range(5000))))
cases.append(("only the marker", b"\xab\xcd\xefMaxMind.com"))

for label, blob in cases:
    db = path("bad.mmdb")
    with open(db, "wb") as f:
        f.write(blob)
    for cmd in readers:
        run([db if a == "DB" else a for a in cmd], f"{cmd[0]} on {label}")

# import: broken inputs of every kind
inputs = {
    "truncated.csv.gz": gzip.compress(b"network,v\n1.2.3.0/24,a\n" * 200)[:300],
    "garbage-after.csv.gz": gzip.compress(b"network,v\n1.2.3.0/24,a\n") + b"not gzip",
    "bad.csv": b'network,v\n1.2.3.0/24,"unterminated\n1.2.4.0/24,b\n',
    "ragged.csv": b"network,v\n1.2.3.0/24\n1.2.4.0/24,b,c\n1.2.5.0/24,ok\n",
    "binary.csv": bytes(rng.randrange(256) for _ in range(4000)),
    "deep.json": b"[" * 5000 + b"]" * 5000,
    "truncated.json": b'{"1.2.3.0/24": {"a": [1, 2, {"b": ',
    "badkeys.json": b'{"1.2.3.0/24": {"a": 1}, 5: 6}',
    "array-of-junk.json": b'[1, "x", null, {"network": "1.2.3.0/24"}, {"network": "zzz"}]',
    "huge-number.json": b'{"1.2.3.0/24": {"n": 1e999999, "m": -99999999999999999999999999999999999999999}}',
    "escapes.json": b'{"1.2.3.0/24": {"s": "\\ud800\\u0000\\uDFFF\\\\\\"\xff\xfe"}}',
    "empty.csv": b"",
    "header-only.csv": b"network,v\n",
    "nul.csv": b"network,v\n1.2.3.0/24,a\x00b\n",
    "long-line.csv": b"network,v\n1.2.3.0/24," + b"x" * 3_000_000 + b"\n",
}
for name, blob in inputs.items():
    with open(path(name), "wb") as f:
        f.write(blob)
    run(["import", "--in", path(name), "--out", path(name + ".mmdb"), "-q"], f"import {name}")
    if os.path.exists(path(name + ".mmdb")):
        run(["verify", "--db", path(name + ".mmdb")], f"verify the import of {name}")

# lookups (joins) that are broken in every way
with open(path("main.csv"), "wb") as f:
    f.write(b"start_ip,end_ip,id\n1.0.0.0,1.0.0.255,a\n2.0.0.0,2.0.0.255,b\n")
lookups = {
    "lk-truncated.csv.gz": gzip.compress(b"id,v\na,1\nb,2\n" * 300)[:200],
    "lk-ragged.csv": b"id,v\na,1\nb\n",
    "lk-unterminated.csv": b'id,v\na,"1\nb,2\n',
    "lk-empty.csv": b"",
    "lk-header-only.csv": b"id,v\n",
    "lk-binary.csv": bytes(rng.randrange(256) for _ in range(3000)),
    "lk-dup.csv": b"id,v,v\na,1,2\na,3,4\n",
}
for name, blob in lookups.items():
    with open(path(name), "wb") as f:
        f.write(blob)
    for extra in ([], ["--lookup-key", "nope"], ["--lookup-on", "nope"], ["--nest-keys", "--type", "v=uint16"]):
        run(["import", path("main.csv"), path(name + ".mmdb"), "--lookup", path(name), "-q", *extra],
            f"import with the lookup {name} {' '.join(extra)}")
# configs with every new key wrong
configs = [
    '{"input": "main.csv", "lookup": 5}', '{"input": "main.csv", "lookup": {"key_column": "id"}}',
    '{"input": "main.csv", "fields": {"rename": ["a"]}}', '{"input": "main.csv", "fields": {"empty_if": {"v": 5}}}',
    '{"input": "main.csv", "metadata": {"languages": "en"}}', '{"input": "main.csv", "shrink": "yes"}',
    '{"input": "main.csv", "tree": {"fill_gaps": 1}}', '{"input": {"path": "main.csv", "columns": "a,b"}}',
]
for i, text in enumerate(configs):
    with open(path(f"cfg{i}.json"), "w") as f:
        f.write(text)
    run(["import", "--config", path(f"cfg{i}.json"), "--out", path(f"cfg{i}.mmdb")], f"config {text}")
# the completion answers on odd words
for words in (["0"], ["5", "x"], ["-3", "read"], ["1", "export", "--format="], ["2", "import", "--type", "=="],
              ["1", "nosuch", "--x"], ["999"], ["0", "\xff\xfe"]):
    run(["__complete", *words], f"__complete {words}")

print("----")
print(f"{runs} runs, {failures} failed")
sys.exit(1 if failures else 0)
