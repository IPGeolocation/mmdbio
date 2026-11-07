#!/usr/bin/env python3
"""End-to-end checks of `mmdbio import` against a brute-force model.

Usage: check_build.py MMDBIO WORKDIR

Each case writes an input file, builds it, exports the result with
`mmdbio export` and compares every address of the test space with the
model: rows in file order, the later row winning where they overlap (or the
earlier one, or merged records, by --merge). Also checks that `mmdbio verify`
accepts every file. Exit status 1 on any failure.
"""
import gzip
import ipaddress
import json
import os
import random
import subprocess
import sys

MMDBIO, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
failures = 0
passed = 0


def run(*args, ok=True):
    p = subprocess.run([MMDBIO, *args], capture_output=True, text=True, errors="backslashreplace")
    if "ERROR: AddressSanitizer" in p.stderr or "runtime error:" in p.stderr or "LeakSanitizer" in p.stderr:
        raise RuntimeError(f"sanitizer report from mmdbio {' '.join(args)}:\n{p.stderr[-4000:]}")
    if ok and p.returncode != 0:
        raise RuntimeError(f"mmdbio {' '.join(args)} failed ({p.returncode}):\n{p.stdout}{p.stderr}")
    return p


def export_map(db):
    """address int -> record, from mmdbio export (IPv4 and IPv6 keys)."""
    out = os.path.join(WORK, "export.json")
    run("export", "--db", db, "--out", out)
    with open(out) as f:
        nets = json.load(f)
    m = {}
    for k, v in nets.items():
        n = ipaddress.ip_network(k)
        m[(n.version, int(n.network_address), int(n.broadcast_address))] = v
    return m


def lookup(m, version, addr):
    for (ver, lo, hi), v in m.items():
        if ver == version and lo <= addr <= hi:
            return v
    return None


def check(name, cond, detail=""):
    global failures, passed
    if cond:
        passed += 1
        print(f"PASS  {name}")
    else:
        failures += 1
        print(f"FAIL  {name}" + (f"\n      {detail}" if detail else ""))


def merge(old, new, deep):
    if isinstance(old, dict) and isinstance(new, dict):
        out = dict(old)
        for k, v in new.items():
            out[k] = merge(old[k], v, True) if deep and k in old else v
        return out
    if deep and isinstance(old, list) and isinstance(new, list):
        return [merge(old[i], new[i], True) if i < len(old) and i < len(new) else (new[i] if i < len(new) else old[i])
                for i in range(max(len(old), len(new)))]
    return new


def model(rows, space, mode="none"):
    """rows: (lo, hi, record) in file order -> address -> expected record"""
    want = {}
    for a in space:
        cover = [r for (lo, hi, r) in rows if lo <= a <= hi]
        if not cover:
            want[a] = None
        elif mode in ("none", "replace"):
            want[a] = cover[-1]
        elif mode == "keep":
            want[a] = cover[0]
        else:
            acc = cover[0]
            for r in cover[1:]:
                acc = merge(acc, r, mode == "recurse") if mode == "recurse" else (
                    {**acc, **r} if isinstance(acc, dict) and isinstance(r, dict) else r)
            want[a] = acc
    return want


def compare(name, db, rows, base, size, version=4, mode="none"):
    run("verify", "--db", db)
    got = export_map(db)
    space = range(base, base + size)
    want = model(rows, space, mode)
    bad = [a for a in space if lookup(got, version, a) != want[a]]
    check(name, not bad, f"{len(bad)} addresses differ, first {ipaddress.ip_address(bad[0]) if bad else ''}: "
          f"got {lookup(got, version, bad[0]) if bad else ''} want {want[bad[0]] if bad else ''}")


def v4(n):
    return str(ipaddress.IPv4Address(n))


rng = random.Random(7)
BASE = int(ipaddress.IPv4Address("10.20.0.0"))
SIZE = 4096

# 1. random overlapping, unsorted ranges in CSV (network column as ranges and CIDRs)
rows = []
lines = ["network,name,score"]
for i in range(400):
    if rng.random() < 0.5:
        lo = BASE + rng.randrange(SIZE)
        hi = min(BASE + SIZE - 1, lo + rng.choice([0, 1, 3, 7, 50, 300, 2000]))
        cell = f"{v4(lo)}-{v4(hi)}"
    else:
        bits = rng.choice([32, 30, 28, 24, 22, 20])
        lo = (BASE + rng.randrange(SIZE)) & ~((1 << (32 - bits)) - 1)
        hi = lo + (1 << (32 - bits)) - 1
        cell = f"{v4(lo)}/{bits}"
    name = f"n{rng.randrange(30)}"
    score = str(rng.randrange(5))
    rows.append((lo, hi, {"name": name, "score": score}))
    lines.append(f"{cell},{name},{score}")
inp = os.path.join(WORK, "overlap.csv")
with open(inp, "w") as f:
    f.write("\n".join(lines) + "\n")
db = os.path.join(WORK, "overlap.mmdb")
run("import", "--in", inp, "--out", db, "-q")
compare("csv: unsorted overlapping rows, the later row wins", db, rows, BASE, SIZE)

for mode in ("keep", "toplevel", "recurse"):
    dbm = os.path.join(WORK, f"overlap-{mode}.mmdb")
    run("import", "--in", inp, "--out", dbm, "--merge", mode, "-q")
    compare(f"csv: --merge {mode}", dbm, rows, BASE, SIZE, mode=mode)

# 2. the same rows gzipped, through a small sort buffer (forces runs on disk)
gz = os.path.join(WORK, "overlap.csv.gz")
with gzip.open(gz, "wt") as f:
    f.write("\n".join(lines) + "\n")
dbg = os.path.join(WORK, "overlap-gz.mmdb")
run("import", "--in", gz, "--out", dbg, "--memory", "16", "-q")
compare("csv.gz: same result", dbg, rows, BASE, SIZE)

# 3. many rows with a tiny buffer: several runs merged
big = ["start_ip,end_ip,v"]
brows = []
for i in range(30000):
    lo = BASE + rng.randrange(SIZE)
    hi = min(BASE + SIZE - 1, lo + rng.choice([0, 0, 1, 5, 40]))
    v = str(rng.randrange(50))
    brows.append((lo, hi, {"v": v}))
    big.append(f"{v4(lo)},{v4(hi)},{v}")
bin_ = os.path.join(WORK, "runs.csv")
with open(bin_, "w") as f:
    f.write("\n".join(big) + "\n")
dbr = os.path.join(WORK, "runs.mmdb")
p = run("import", "--in", bin_, "--out", dbr, "--memory", "16")
compare("start/end columns, 30,000 rows", dbr, brows, BASE, SIZE)

# 4. JSON forms: keyed object, array of rows, NDJSON, gzipped
recs = [(BASE + i * 64, BASE + i * 64 + 63, {"i": i, "ok": i % 2 == 0, "x": i / 4, "tags": ["a", str(i)]})
        for i in range(32)]
keyed = {f"{v4(lo)}/26": r for lo, hi, r in recs}
arr = [{"network": f"{v4(lo)}/26", **r} for lo, hi, r in recs]
for fname, text in [("keyed.json", json.dumps(keyed)), ("rows.json", json.dumps(arr)),
                    ("rows.ndjson", "\n".join(json.dumps(r) for r in arr))]:
    pth = os.path.join(WORK, fname)
    with open(pth, "w") as f:
        f.write(text)
    for suffix in ("", ".gz"):
        src = pth
        if suffix:
            src = pth + ".gz"
            with gzip.open(src, "wt") as f:
                f.write(text)
        dbj = os.path.join(WORK, fname + suffix + ".mmdb")
        run("import", "--in", src, "--out", dbj, "-q")
        compare(f"json: {fname}{suffix}", dbj, recs, BASE, SIZE)

# 5. JSON types are kept: uint32, boolean, double, arrays
dbj = os.path.join(WORK, "keyed.json.mmdb")
info = run("inspect", "--db", dbj, "--sample-ip", v4(BASE + 70)).stdout
check("json: types kept (uint64 reader type for integers, bool, float64)",
      "i  " in info.replace(" ", " ") and "bool" in info and "float64" in info, info)

# 6. CSV types and inference
typed = os.path.join(WORK, "typed.csv")
with open(typed, "w") as f:
    f.write("cidr;count;ratio;flag;names;zip\n10.20.0.0/24;12;0.5;true;a|b;007\n")
dbt = os.path.join(WORK, "typed.mmdb")
run("import", "--in", typed, "--out", dbt, "--type", "count=uint16", "--type", "ratio=double",
    "--type", "flag=boolean", "--type", "names=array", "--array-separator", "|", "-q")
r = json.loads(run("read", "--db", dbt, "--ip", "10.20.0.9").stdout)
check("csv: semicolon detected, typed fields", r == {"count": 12, "ratio": 0.5, "flag": True, "names": ["a", "b"],
                                                      "zip": "007"}, str(r))
dbi = os.path.join(WORK, "infer.mmdb")
run("import", "--in", typed, "--out", dbi, "--infer-types", "-q")
r = json.loads(run("read", "--db", dbi, "--ip", "10.20.0.9").stdout)
check("csv: --infer-types", r == {"count": 12, "ratio": 0.5, "flag": True, "names": "a|b", "zip": "007"}, str(r))

# 7. no header, integer IPs, TSV, IPv6, nested keys
nh = os.path.join(WORK, "noheader.tsv")
with open(nh, "w") as f:
    f.write(f"{BASE}\t{BASE + 255}\tAU\n{BASE + 256}\t{BASE + 511}\tNZ\n")
dbn = os.path.join(WORK, "noheader.mmdb")
run("import", "--in", nh, "--out", dbn, "-q")
r = json.loads(run("read", "--db", dbn, "--ip", v4(BASE + 300)).stdout)
check("tsv without a header, integer IPs", r == {"column3": "NZ"}, str(r))
v6 = os.path.join(WORK, "v6.csv")
with open(v6, "w") as f:
    f.write("network,location.country,location.city\n2001:db8::/48,DE,Berlin\n10.20.0.0/24,AU,Sydney\n")
db6 = os.path.join(WORK, "v6.mmdb")
run("import", "--in", v6, "--out", db6, "--nest-keys", "-q")
r = json.loads(run("read", "--db", db6, "--ip", "2001:db8::1").stdout)
check("ipv6 + nested keys", r == {"location": {"country": "DE", "city": "Berlin"}}, str(r))
r = json.loads(run("read", "--db", db6, "--ip", "10.20.0.1").stdout)
check("ipv4 in an ipv6 database", r == {"location": {"country": "AU", "city": "Sydney"}}, str(r))

# 8. aliasing makes ::ffff:a.b.c.d and 6to4 find the IPv4 data
dba = os.path.join(WORK, "alias.mmdb")
run("import", "--in", v6, "--out", dba, "--alias-6to4", "-q")
r = json.loads(run("read", "--db", dba, "--ip", "2002:0a14:0001::").stdout)
check("--alias-6to4: 6to4 address finds the IPv4 row", r == {"location.country": "AU", "location.city": "Sydney"}, str(r))
run("verify", "--db", dba)

# 9. bad rows are skipped with a warning; --strict stops
bad = os.path.join(WORK, "bad.csv")
with open(bad, "w") as f:
    f.write("network,v\n10.20.0.0/24,a\nnot-an-ip,b\n10.20.1.0/24,c\n10.20.2.5-10.20.2.1,d\n")
dbb = os.path.join(WORK, "bad.mmdb")
p = run("import", "--in", bad, "--out", dbb)
check("bad rows skipped with warnings", "invalid network" in p.stderr and "starts after it ends" in p.stderr
      and "2 skipped" in p.stderr, p.stderr)
p = run("import", "--in", bad, "--out", dbb, "--strict", ok=False)
check("--strict stops at the first bad row", p.returncode == 1 and "bad.csv:3: invalid network" in p.stderr, p.stderr)
p = run("import", "--in", bad, "--out", dbb, "--type", "v=uint16", ok=True)
check("a value its type rejects skips the row", "cannot read" in p.stderr, p.stderr)

# 10. a config file, overridden by flags
cfg = os.path.join(WORK, "build.json")
with open(cfg, "w") as f:
    json.dump({"input": "typed.csv", "output": "from-config.mmdb",
               "metadata": {"database_type": "Cfg-DB", "description": {"en": "From config", "de": "Aus Konfig"},
                            "languages": ["en", "de"], "record_size": 24},
               "fields": {"types": {"count": "uint32"}, "exclude": ["zip"]}}, f)
run("import", "--config", cfg, "-q")
dbc = os.path.join(WORK, "from-config.mmdb")
meta = run("metadata", "--db", dbc).stdout
r = json.loads(run("read", "--db", dbc, "--ip", "10.20.0.1").stdout)
check("config: paths relative to the config, metadata, types, exclude",
      "Cfg-DB" in meta and "24 bits" in meta and "[en de]" in meta and r.get("count") == 12 and "zip" not in r,
      meta + str(r))
run("import", "--config", cfg, "--title", "Flag-DB", "--size", "32", "-q")
meta = run("metadata", "--db", dbc).stdout
check("config: flags override it", "Flag-DB" in meta and "32 bits" in meta, meta)

# 11. ip version 4 rejects IPv6 rows
db4 = os.path.join(WORK, "v4only.mmdb")
p = run("import", "--in", v6, "--out", db4, "--ip", "4")
check("--ip 4 skips IPv6 rows", "IPv6 network in an IPv4 database" in p.stderr, p.stderr)

# 12. diff between two builds
dbd = os.path.join(WORK, "diff2.mmdb")
with open(os.path.join(WORK, "diff2.json"), "w") as f:
    k2 = dict(keyed)
    k2.pop(f"{v4(BASE)}/26")
    k2[f"{v4(BASE + 64)}/26"] = {"changed": True}
    k2["10.30.0.0/24"] = {"new": 1}
    json.dump(k2, f)
run("import", "--in", os.path.join(WORK, "diff2.json"), "--out", dbd, "-q")
s = run("diff", "--old", dbj, "--new", dbd, "--summary").stdout.strip()
check("diff --summary", s == "Added: 1 | Removed: 1 | Modified: 1", s)
dj = json.loads(run("diff", "--old", dbj, "--new", dbd, "--json").stdout)
check("diff --json", list(dj) == ["added", "modified", "removed"] and "10.30.0.0/24" in dj["added"] and
      dj["modified"][f"{v4(BASE + 64)}/26"]["new"] == {"changed": True}, json.dumps(dj)[:300])

print("----")
print(f"{passed} passed, {failures} failed")
sys.exit(1 if failures else 0)
