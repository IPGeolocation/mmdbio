"""Edge cases, end to end: databases built with every import feature and
input shape, each checked lookup by lookup with an independent reader
(mmdb_py.Reader), then read back in every format, exported, re-imported,
shrunk, compared and verified.

    python3 check_edges.py path/to/mmdbio workdir
"""
import csv
import gzip
import io
import ipaddress
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mmdb_py import Reader  # noqa: E402

M = os.path.abspath(sys.argv[1])
work = sys.argv[2]
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
os.chdir(work)
passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("PASS ", name)
    else:
        failed += 1
        print("FAIL ", name)
        if detail != "":
            print("      " + str(detail)[:2000].replace("\n", "\n      "))


def run(*args, stdin=None):
    p = subprocess.run([M, *args], input=stdin, capture_output=True)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def write(path, text, gz=False):
    data = text.encode() if isinstance(text, str) else text
    if gz:
        data = gzip.compress(data)
    with open(path, "wb") as f:
        f.write(data)


def f32(x):
    return struct.unpack(">f", struct.pack(">f", x))[0]


def lookups_match(db, expect, name):
    """expect: {ip: record or None}"""
    r = Reader(db)
    bad = [(ip, want, r.lookup(ip)) for ip, want in expect.items() if r.lookup(ip) != want]
    check(name, not bad, bad[:3])
    return r


def round_trips(db, name, strings_only=False, v4=False, types=()):
    """read, export, re-import, shrink, diff and verify one database; JSON
    can't say uint16 or float, so typed data goes back with its --type rules"""
    r = Reader(db)
    rc, out, err = run("verify", db)
    check(f"{name}: verify", rc == 0 and out.strip() == "valid", out + err)
    rc, js, err = run("export", db, "-f", "jsonl")
    rows = [json.loads(x) for x in js.splitlines()] if rc == 0 else []
    bad = []
    for row in rows[:3000]:
        net = ipaddress.ip_network(row.pop("range"))
        want = r.lookup(str(net.network_address))
        if isinstance(want, dict) and "range" in want:
            want = dict(want)
            want.pop("range")
        if not isinstance(want, dict):
            want = {"value": want}
        if row != want and not floats_close(row, want):
            bad.append((str(net), row, want))
    check(f"{name}: export jsonl agrees with the reader", rc == 0 and not bad, bad[:2] or err)
    # read: every format agrees on a sample
    sample = [str(ipaddress.ip_network(x["range"] if "range" in x else "0.0.0.0/32").network_address)
              for x in [json.loads(y) for y in js.splitlines()[:40]]]
    if sample:
        rc1, jl, _ = run("read", *sample, db, "-f", "jsonl")
        rc2, tsv, _ = run("read", *sample, db, "-f", "tsv", "--flatten")
        rc3, cs, _ = run("read", *sample, db, "-f", "csv")
        jl_rows = [json.loads(x) for x in jl.splitlines()]
        check(f"{name}: read jsonl, csv and tsv give a row per address",
              rc1 == rc2 == rc3 == 0 and len(jl_rows) == len(sample) == len(list(csv.reader(io.StringIO(cs)))) - 1
              == len(tsv.splitlines()) - 1, (rc1, rc2, rc3, len(jl_rows), len(sample)))
    # export -> import -> the same lookups
    fmt = "csv" if strings_only else "jsonl"
    if fmt == "csv":
        rc, _, err = run("export", db, f"{name}.rt.csv", "--flatten")
    else:
        rc, _, err = run("export", db, f"{name}.rt.jsonl")
    args = ["import", f"{name}.rt.{fmt}", f"{name}.rt.mmdb", "-q", *types] + (["--ip", "4"] if v4 else [])
    if fmt == "csv":
        args += ["--nest-keys"]
    rc2, _, err2 = run(*args)
    if rc == 0 and rc2 == 0:
        rc3, out, _ = run("diff", db, f"{name}.rt.mmdb", "--summary")
        check(f"{name}: export {fmt} and import back: the same networks and records",
              out.strip() == "Added: 0 | Removed: 0 | Modified: 0", out)
    else:
        check(f"{name}: export {fmt} and import back", False, err + err2)
    # shrink, then everything still answers the same
    rc, out, err = run("shrink", db, f"{name}.small.mmdb", "--overwrite", "--full-check", "--lookups", "3000",
                       "--bench=false")
    check(f"{name}: shrink checks out", rc == 0 and "[OK] all networks" in out, out[-600:] + err)
    if rc == 0:
        rc, out, _ = run("diff", db, f"{name}.small.mmdb", "-s", "-r", "--summary")
        check(f"{name}: diff with the shrunk copy is empty", out.strip() == "Subnets changed: 0 | Records changed: 0", out)
        s = Reader(f"{name}.small.mmdb")
        rnd = random.Random(1)
        ips = [str(ipaddress.IPv4Address(rnd.getrandbits(32))) for _ in range(500)]
        if r.meta["ip_version"] == 6:
            ips += [str(ipaddress.IPv6Address(rnd.getrandbits(128))) for _ in range(200)]
        check(f"{name}: the independent reader agrees on the shrunk copy", all(r.lookup(i) == s.lookup(i) for i in ips))
    rc, out, _ = run("metadata", db, "--json", "--data-types")
    m = json.loads(out) if rc == 0 else {}
    check(f"{name}: metadata data types add up to the data section",
          rc == 0 and sum(t["bytes"] for t in m.get("data_types", {}).values()) == m["layout"]["data_section_size"], out[:500])


def floats_close(a, b):
    if isinstance(a, float) and isinstance(b, float):
        return a == b or abs(a - b) <= 1e-6 * max(abs(a), abs(b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(floats_close(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(floats_close(x, y) for x, y in zip(a, b))
    return a == b


# ---- 1. every value type, from JSON ----
big = "x" * 70000
rec = {
    "s": "plain", "empty": "", "uni": "Zürich 中文 \U0001f600", "len285": "a" * 285, "len65821": "b" * 65821,
    "big": big, "u16": 65535, "u32": 4294967295, "u64": 18446744073709551615, "u128": 2 ** 128 - 1,
    "i32min": -2147483648, "i32max": 2147483647, "zero": 0, "d": 0.1, "dbig": 1e300, "dtiny": 5e-324,
    "dneg": -2.5, "f": 1.5, "t": True, "fa": False, "arr": [1, "two", [3, [4]], {"five": 5}], "earr": [],
    "map": {"a": {"b": {"c": {"d": {"e": "deep"}}}}}, "emap": {},
}
types = ["--type", "u16=uint16", "--type", "u32=uint32", "--type", "u64=uint64", "--type", "u128=uint128",
         "--type", "f=float", "--type", "zero=int32"]
rows = [dict(rec, network="1.2.3.0/24"), dict(rec, network="2001:db8::/48", s="v6")]
write("types.jsonl", "\n".join(json.dumps(r) for r in rows) + "\n")
rc, _, err = run("import", "types.jsonl", "types.mmdb", "-q", *types)
want = dict(rec)
want["f"] = f32(1.5)
r = lookups_match("types.mmdb", {"1.2.3.4": want, "2001:db8::1": dict(want, s="v6"), "1.2.4.0": None},
                  "types: every MMDB type round-trips exactly")
rc, out, _ = run("read", "1.2.3.4", "types.mmdb", "-f", "jsonl", "--fields", "len285,len65821")
j = json.loads(out) if rc == 0 else {}
check("types: the 285- and 65,821-byte strings (the Perl writer's bad lengths) read back whole",
      len(j.get("len285", "")) == 285 and len(j.get("len65821", "")) == 65821)
rc, out, _ = run("metadata", "types.mmdb", "--json", "--data-types")
dt = json.loads(out).get("data_types", {}) if rc == 0 else {}
check("types: metadata --data-types counts every type used",
      all(t in dt for t in ("string", "uint16", "uint32", "uint64", "uint128", "int32", "double", "float", "boolean",
                            "array", "map")), sorted(dt))
round_trips("types.mmdb", "types", types=types)

# ---- 2. the same types from CSV, by --type and --infer-types ----
write("typed.csv", "network,u16,i32,d,b,arr,s\n10.0.0.0/8,65535,-7,2.5,yes,\"[a, 'b', \"\"c\"\"]\",007\n")
run("import", "typed.csv", "typed.mmdb", "-q", "--type", "u16=uint16", "--type", "i32=int32", "--type", "d=double",
    "--type", "b=boolean", "--type", "arr=array")
lookups_match("typed.mmdb", {"10.1.2.3": {"u16": 65535, "i32": -7, "d": 2.5, "b": True, "arr": ["a", "b", "c"],
                                          "s": "007"}}, "csv: typed columns")
run("import", "typed.csv", "inferred.mmdb", "-q", "--infer-types")
lookups_match("inferred.mmdb", {"10.1.2.3": {"u16": 65535, "i32": -7, "d": 2.5, "b": "yes",
                                             "arr": "[a, 'b', \"c\"]", "s": "007"}},
              "csv: --infer-types (007 stays a string, yes is not a boolean)")
rc, _, err = run("import", "typed.csv", "bad.mmdb", "--type", "u16=uint16", "--type", "d=uint16", "--strict")
check("csv: a value its type rejects fails with --strict", rc == 1 and "typed.csv:2" in err, err)

# ---- 3. network edges ----
lines = ["network,v",
         "0.0.0.0/32,first4", "255.255.255.255/32,last4", "1.2.3.7-1.2.4.9,unaligned", "16909060,integer",
         "::ffff:5.6.7.8,mapped", "::/128,first6", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128,last6",
         "2001:db8::/32,doc", "9.9.9.9 - 9.9.9.12,spaced"]
write("edges.csv", "\n".join(lines) + "\n")
rc, _, err = run("import", "edges.csv", "edges.mmdb", "-q")
check("networks: every edge form imports", rc == 0, err)
# 0.0.0.0 is :: in an IPv6 tree, so the later ::/128 row wins there
lookups_match("edges.mmdb", {
    "0.0.0.0": {"v": "first6"}, "0.0.0.1": None, "255.255.255.255": {"v": "last4"}, "255.255.255.254": None,
    "1.2.3.6": None, "1.2.3.7": {"v": "unaligned"}, "1.2.4.9": {"v": "unaligned"}, "1.2.4.10": None,
    "1.2.3.4": {"v": "integer"}, "5.6.7.8": {"v": "mapped"}, "::": {"v": "first6"}, "::1": None,
    "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff": {"v": "last6"}, "2001:db8:ffff::": {"v": "doc"},
    "9.9.9.9": {"v": "spaced"}, "9.9.9.12": {"v": "spaced"}, "9.9.9.13": None}, "networks: lookups at every edge")
round_trips("edges.mmdb", "edges", strings_only=True)
write("all.csv", "network,v\n0.0.0.0/0,everything4\n")
run("import", "all.csv", "all4.mmdb", "-q", "--ip", "4")
lookups_match("all4.mmdb", {"0.0.0.0": {"v": "everything4"}, "255.255.255.255": {"v": "everything4"}},
              "networks: 0.0.0.0/0 in an IPv4 database")
write("all6.csv", "network,v\n::/0,everything6\n")
run("import", "all6.csv", "all6.mmdb", "-q")
lookups_match("all6.mmdb", {"::": {"v": "everything6"}, "8.8.8.8": {"v": "everything6"},
                            "ffff::1": {"v": "everything6"}}, "networks: ::/0 in an IPv6 database")
round_trips("all6.mmdb", "all6", strings_only=True)

# ---- 4. aliasing and reserved networks ----
write("al.csv", "network,v\n8.8.8.0/24,google\n10.1.0.0/16,private\n2a00::/16,v6\n")
run("import", "al.csv", "alias.mmdb", "-q", "--alias-6to4")
lookups_match("alias.mmdb", {"8.8.8.8": {"v": "google"}, "::ffff:8.8.8.8": {"v": "google"},
                             "2002:808:808::": {"v": "google"}, "2001:0:808:808::": {"v": "google"},
                             "2002:909:909::": None, "2a00::1": {"v": "v6"}}, "aliasing: the three IPv4 aliases")
run("import", "al.csv", "noalias.mmdb", "-q")
lookups_match("noalias.mmdb", {"8.8.8.8": {"v": "google"}, "2002:808:808::": None}, "aliasing: off by default")
run("import", "al.csv", "noreserved.mmdb", "-q", "--disallow-reserved")
lookups_match("noreserved.mmdb", {"8.8.8.8": {"v": "google"}, "10.1.2.3": None}, "--disallow-reserved drops 10/8")
round_trips("alias.mmdb", "alias", strings_only=True)
rc, out, _ = run("export", "alias.mmdb", "-f", "cidr", "--aggregate=false")
check("aliasing: export lists the aliased networks once", "2002" not in out and "::ffff" not in out and
      "8.8.8.0/24" in out, out)

# ---- 5. record sizes ----
lines = ["network,v"] + [f"{i >> 16 & 255}.{i >> 8 & 255}.{i & 255}.0/24,r{i % 7}" for i in range(1, 3000)]
write("sizes.csv", "\n".join(lines) + "\n")
for size in ("24", "28", "32"):
    run("import", "sizes.csv", f"s{size}.mmdb", "-q", "--size", size)
    check(f"--size {size}", Reader(f"s{size}.mmdb").rs == int(size))
run("import", "sizes.csv", "sauto.mmdb", "-q")
check("--size auto picks 24 when it fits", Reader("sauto.mmdb").rs == 24)
rc, out, _ = run("diff", "s24.mmdb", "s32.mmdb", "--summary")
check("record sizes change nothing else", out.strip() == "Added: 0 | Removed: 0 | Modified: 0", out)
write("huge.jsonl", "\n".join(json.dumps({"network": f"10.{i}.0.0/16", "blob": chr(65 + i % 26) * 600000 + str(i)})
                              for i in range(30)) + "\n")
rc, _, err = run("import", "huge.jsonl", "huge.mmdb", "-q", "--ip", "4")
check("--size auto moves to 28 past 16 MB of data", rc == 0 and Reader("huge.mmdb").rs == 28, err)
rc, _, err = run("import", "huge.jsonl", "huge24.mmdb", "-q", "--ip", "4", "--size", "24")
check("--size 24 that can't fit fails", rc == 1 and err.strip(), err)

# ---- 6. CSV dialects ----
dialects = {
    "semicolon": "network;name;note\n1.0.0.0/24;a;x\n",
    "pipe": "network|name|note\n1.0.0.0/24|a|x\n",
    "tab": "network\tname\tnote\n1.0.0.0/24\ta\tx\n",
    "crlf": "network,name,note\r\n1.0.0.0/24,a,x\r\n",
    "bom": "﻿network,name,note\n1.0.0.0/24,a,x\n",
    "quoted": 'network,name,note\n"1.0.0.0/24","a","x"\n',
}
for d, text in dialects.items():
    write(f"d-{d}.csv", text)
    rc, _, err = run("import", f"d-{d}.csv", f"d-{d}.mmdb", "-q")
    check(f"csv dialect: {d}", rc == 0 and Reader(f"d-{d}.mmdb").lookup("1.0.0.1") == {"name": "a", "note": "x"}, err)
write("q.csv", 'network,v\n1.0.0.0/24,"comma, ""quote"" and\nnewline"\n')
run("import", "q.csv", "q.mmdb", "-q")
lookups_match("q.mmdb", {"1.0.0.1": {"v": 'comma, "quote" and\nnewline'}}, "csv: quotes, delimiters and newlines in a field")
write("lazy.csv", 'network,v\n1.0.0.0/24,a "stray" quote\n')
rc, _, _ = run("import", "lazy.csv", "lazy.mmdb", "-q", "--strict")
rc2, _, _ = run("import", "lazy.csv", "lazy.mmdb", "-q", "--lazy-quotes")
check("csv: stray quotes need --lazy-quotes", rc == 1 and rc2 == 0 and
      Reader("lazy.mmdb").lookup("1.0.0.1") == {"v": 'a "stray" quote'})
write("dup.csv", "network,v,v,\n1.0.0.0/24,first,second,third\n")
run("import", "dup.csv", "dup.mmdb", "-q")
lookups_match("dup.mmdb", {"1.0.0.1": {"v": "second", "column4": "third"}},
              "csv: a repeated column keeps the later one; an unnamed one is columnN")
write("se.csv", "from,to,v\n1.0.0.0,1.0.0.255,a\n")
run("import", "se.csv", "se.mmdb", "-q", "--start-column", "from", "--end-column", "to")
lookups_match("se.mmdb", {"1.0.0.9": {"v": "a"}}, "csv: --start-column and --end-column")
write("nc.csv", "v,where\na,1.0.0.0/24\n")
run("import", "nc.csv", "nc.mmdb", "-q", "--network-column", "where")
lookups_match("nc.mmdb", {"1.0.0.9": {"v": "a"}}, "csv: --network-column")
write("wide.csv", "network," + ",".join(f"c{i}" for i in range(1000)) + "\n1.0.0.0/24," +
      ",".join(str(i) for i in range(1000)) + "\n")
run("import", "wide.csv", "wide.mmdb", "-q")
w = Reader("wide.mmdb").lookup("1.0.0.1")
check("csv: 1,000 columns", w and len(w) == 1000 and w["c999"] == "999")

# ---- 7. gzip, stdin, stdout ----
write("g.csv.gz", "network,v\n1.0.0.0/24,gz\n", gz=True)
write("multi.gz", gzip.compress(b"network,v\n1.0.0.0/24,a\n") + gzip.compress(b"2.0.0.0/24,b\n"))
run("import", "g.csv.gz", "g.mmdb", "-q")
run("import", "multi.gz", "multi.mmdb", "-q", "--format", "csv")
lookups_match("g.mmdb", {"1.0.0.1": {"v": "gz"}}, "gzip input")
lookups_match("multi.mmdb", {"1.0.0.1": {"v": "a"}, "2.0.0.1": {"v": "b"}}, "concatenated gzip members")
write("g.jsonl.gz", '{"network":"1.0.0.0/24","v":1}\n', gz=True)
run("import", "g.jsonl.gz", "gj.mmdb", "-q")
lookups_match("gj.mmdb", {"1.0.0.1": {"v": 1}}, "gzipped NDJSON")
p = subprocess.run([M, "import", "-", "--out", "-", "-q"], input=gzip.compress(b"network,v\n1.0.0.0/24,s\n"),
                   capture_output=True)
write("stdio.mmdb", p.stdout)
lookups_match("stdio.mmdb", {"1.0.0.1": {"v": "s"}}, "gzipped stdin to stdout")

# ---- 8. bad rows ----
write("badrows.csv", "network,v\n1.0.0.0/24,ok\nnot-an-ip,x\n2.0.0.9-2.0.0.1,backwards\n3.0.0.0/24,ok3\n")
rc, _, err = run("import", "badrows.csv", "br.mmdb")
check("bad rows are skipped with warnings", rc == 0 and "2 skipped" in err and "badrows.csv:3" in err, err)
lookups_match("br.mmdb", {"1.0.0.1": {"v": "ok"}, "3.0.0.1": {"v": "ok3"}}, "the good rows around them stay")
rc, _, err = run("import", "badrows.csv", "br2.mmdb", "--strict")
check("--strict stops at the first bad row, naming it", rc == 1 and "badrows.csv:3" in err, err)
write("v6in4.csv", "network,v\n2001:db8::/32,x\n")
rc, _, err = run("import", "v6in4.csv", "v6in4.mmdb", "--ip", "4", "--strict")
check("an IPv6 network in an IPv4 database is refused", rc == 1 and "IPv6" in err, err)
write("hdr.csv", "network,v\n")
rc, _, err = run("import", "hdr.csv", "hdr.mmdb", "-q")
check("a header with no rows builds an empty, valid database",
      rc == 0 and run("verify", "hdr.mmdb")[1].strip() == "valid", err)
write("empty.csv", "")
rc, _, err = run("import", "empty.csv", "e.mmdb")
check("an empty input is refused", rc == 1 and "empty" in err, err)
write("badutf.csv", b"network,v\n1.0.0.0/24,ab\xff\xfecd\n")
run("import", "badutf.csv", "badutf.mmdb", "-q")
lookups_match("badutf.mmdb", {"1.0.0.1": {"v": "ab��cd"}}, "invalid UTF-8 becomes U+FFFD")

# ---- 9. merges with overlaps ----
write("ov.jsonl", "\n".join(json.dumps(x) for x in [
    {"network": "10.0.0.0/8", "a": 1, "m": {"x": 1, "y": 1}, "l": [1, 2, 3]},
    {"network": "10.1.0.0/16", "b": 2, "m": {"y": 2, "z": 2}, "l": [9]},
    {"network": "10.1.1.0/24", "a": 3},
]) + "\n")
expect = {
    "none": {"10.0.0.1": {"a": 1, "m": {"x": 1, "y": 1}, "l": [1, 2, 3]}, "10.1.0.1": {"b": 2, "m": {"y": 2, "z": 2}, "l": [9]},
             "10.1.1.1": {"a": 3}},
    "keep": {"10.0.0.1": {"a": 1, "m": {"x": 1, "y": 1}, "l": [1, 2, 3]},
             "10.1.0.1": {"a": 1, "m": {"x": 1, "y": 1}, "l": [1, 2, 3]}, "10.1.1.1": {"a": 1, "m": {"x": 1, "y": 1}, "l": [1, 2, 3]}},
    "toplevel": {"10.1.0.1": {"a": 1, "b": 2, "m": {"y": 2, "z": 2}, "l": [9]},
                 "10.1.1.1": {"a": 3, "b": 2, "m": {"y": 2, "z": 2}, "l": [9]}},
    "recurse": {"10.1.0.1": {"a": 1, "b": 2, "m": {"x": 1, "y": 2, "z": 2}, "l": [9, 2, 3]},
                "10.1.1.1": {"a": 3, "b": 2, "m": {"x": 1, "y": 2, "z": 2}, "l": [9, 2, 3]}},
}
for mode, want in expect.items():
    run("import", "ov.jsonl", f"ov-{mode}.mmdb", "-q", "--merge", mode, "--ip", "4")
    lookups_match(f"ov-{mode}.mmdb", want, f"--merge {mode}")
    round_trips(f"ov-{mode}.mmdb", f"ov-{mode}", v4=True)

# ---- 10. the shaping options together ----
write("shape.csv", "start,end,id,kind,score,loc.city,loc.cc\n1.0.0.0,1.0.0.255,7,vpn,0,Paris,FR\n"
                   "2.0.0.0,2.0.0.9,8,0,5,,DE\n")
run("import", "shape.csv", "shape.mmdb", "-q", "--ip", "4", "--nest-keys", "--network-field", "--fill-gaps",
    "--merge", "toplevel", "--rename", "kind=type", "--empty-if", "kind=0", "--type", "score=uint16",
    "--exclude", "id")
lookups_match("shape.mmdb", {
    "1.0.0.1": {"type": "vpn", "score": 0, "loc": {"city": "Paris", "cc": "FR"}, "network": "1.0.0.0/24"},
    "2.0.0.5": {"type": "", "score": 5, "loc": {"city": "", "cc": "DE"}, "network": "2.0.0.0-2.0.0.9"},
    "3.0.0.1": {"type": "", "score": "", "loc": {"city": "", "cc": ""}}}, "rename, empty-if, nest-keys, network field, "
              "fill-gaps and toplevel merge together")
run("import", "shape.csv", "nof.mmdb", "-q", "--ip", "4", "--no-fields", "--network-field")
lookups_match("nof.mmdb", {"1.0.0.1": {"network": "1.0.0.0/24"}, "3.0.0.1": None}, "--no-fields with --network-field")
write("hl.csv", "1.0.0.0,1.0.0.255,k,DE\n")
run("import", "hl.csv", "mc.mmdb", "-q", "--ip", "4", "--joinkey-col", "--fields", "country")
lookups_match("mc.mmdb", {"1.0.0.1": {"country": "DE"}}, "--joinkey-col --fields")
run("import", "hl.csv", "cols.mmdb", "-q", "--ip", "4", "--columns", "a,b,c,d", "--start-column", "a",
    "--end-column", "b", "--exclude", "c")
lookups_match("cols.mmdb", {"1.0.0.1": {"d": "DE"}}, "--columns names a headerless CSV")
run("import", "shape.csv", "fg6.mmdb", "-q", "--fill-gaps", "--disallow-reserved", "--include", "kind")
lookups_match("fg6.mmdb", {"8.8.8.8": {"kind": ""}, "2a00::1": {"kind": ""}, "10.0.0.1": None,
                           "1.0.0.1": {"kind": "vpn"}}, "--fill-gaps covers IPv6 too and leaves reserved space out")

# ---- 11. lookups (joins) ----
write("main.csv", "start_ip,end_ip,lid\n1.0.0.0,1.0.0.255,a\n2.0.0.0,2.0.0.255,b\n3.0.0.0,3.0.0.255,zz\n4.0.0.0,4.0.0.255,\n")
write("lk.tsv", "id\tname\tcc\tscore\na\tA\tUS\t1\nb\tB\t0\t2\na\tA2\tDE\t3\n\tblank\tFR\t4\n")
write("lk.tsv.gz", open("lk.tsv").read(), gz=True)
for lk in ("lk.tsv", "lk.tsv.gz"):
    rc, _, err = run("import", "main.csv", f"j-{lk}.mmdb", "--ip", "4", "--lookup", lk, "--lookup-on", "lid",
                     "--type", "score=uint16", "--empty-if", "cc=0", "--rename", "cc=country")
    check(f"lookup {lk}: builds, counting the ids it lacks", rc == 0 and "1 rows have an id the lookup lacks" in err, err)
    lookups_match(f"j-{lk}.mmdb", {"1.0.0.1": {"name": "A2", "country": "DE", "score": 3},
                                   "2.0.0.1": {"name": "B", "country": "", "score": 2},
                                   "3.0.0.1": {"name": "", "country": ""},
                                   "4.0.0.1": {"name": "blank", "country": "FR", "score": 4}},
                  f"lookup {lk}: last row wins, a missing id is all empty, an empty id is a key")

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
