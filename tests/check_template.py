"""Record templates, end to end: the "record" object of an import config
(tables joined by id, nested joins, define/$use, value steps, literal and
list parsing, empty and invalid values, skip rules), big table values kept
once (shared values) under every merge mode, "order", "vars" with --var, and
inputs found as .gz. Every database is read back with the
independent reader (mmdb_py.Reader).

    python3 check_template.py path/to/mmdbio workdir
"""
import gzip
import json
import os
import shutil
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import mmdb_py  # noqa: E402

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
        if detail:
            print("      " + str(detail)[:1500].replace("\n", "\n      "))


def run(*args, env=None):
    p = subprocess.run([M, *args], capture_output=True, env=env)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def write(path, text):
    with open(path, "w", newline="", encoding="utf-8") as f:
        f.write(text)


def config(path, cfg):
    write(path, json.dumps(cfg, indent=1))


def build(cfg, *flags, name="t.json"):
    config(name, cfg)
    return run("import", "--config", name, *flags)


def look(db, ip):
    return mmdb_py.Reader(db).lookup(ip)


# ---- a city database: two tables, joined inside each other ----
write("country.csv", "id,code2,name_place_id,continent\n1, US ,20,NA\n2,ZZ,21,\n3,ca,22,NA\n")
write("place.csv", "id,place_english,place_german\n10,San Jose , San José\n11,Toronto,\n"
      "20,United States,Vereinigte Staaten\n22,Canada,Kanada\n")
write("ranges.csv",
      "start_ip,end_ip,country_id,city_place_id,state_code,lat,radius,routes,peers,flags,score\n"
      "1.0.0.0,1.0.0.255,1,10, us-ca ,37.5,12.2,\"['1.0.0.0/24', '2001:db8::/32']\","
      "\"[{'as_number': 13335, 'name': 'Cloud\\xa0flare'}]\",\"[A, 'B',,]\",7\n"
      "2001:db8::,2001:db8::ffff,1,11,CA-ON,91,,[],[],[],\n"
      "2.0.0.0,2.0.0.255,2,10,,1,1,[],[],,1\n"
      "3.0.0.0,3.0.0.255,9,10,,1,1,[],[],,1\n"
      "4.0.0.0,4.0.0.255,3,99,ON,x,70000,[],[],,abc\n")
city = {
    "input": "ranges.csv", "output": "city.mmdb",
    "metadata": {"database_type": "T", "record_size": 32},
    "record": {
        "tables": {"country": "country.csv", "place": {"path": "place.csv", "key": "id", "trim": False}},
        "trim": True,
        "types": {"latitude": "double", "accuracy_radius": "uint16", "as_number": "string", "score": "uint16"},
        "define": {"names": {"en": "place_english", "de": "place_german"}},
        "skip_if": [{"value": {"$table": "country", "$on": "country_id", "$col": "code2"}, "in": ["", "ZZ"]}],
        "shape": {
            "country": {"$table": "country", "$on": "country_id",
                        "iso_code": {"$col": "code2", "$case": "upper"},
                        "names": {"$use": "names", "$table": "place", "$on": "name_place_id"},
                        "continent": {"$col": "continent", "$one_of": ["NA", "EU"], "$invalid": "omit"}},
            "city": {"names": {"$use": "names", "$table": "place", "$on": "city_place_id", "$missing": "omit"}},
            "state": {"$col": "state_code", "$case": "upper", "$empty": "omit", "$invalid": "omit",
                      "$strip_prefix": {"equals": {"$table": "country", "$on": "country_id", "$col": "code2",
                                                   "$case": "upper"}},
                      "$chars": "A-Z0-9", "$length": [1, 3]},
            "latitude": {"$col": "lat", "$number": [-90, 90], "$invalid": "omit"},
            "accuracy_radius": {"$col": "radius", "$number": [0, 65535], "$ceil": True, "$invalid": "omit"},
            "routes": {"$col": "routes", "$parse": "literal"},
            "peers": {"$col": "peers", "$parse": "literal"},
            "flags": {"$col": "flags", "$parse": "list"},
            "score": {"$col": "score", "$default": "0", "$invalid": "omit"},
            "fixed": {"$const": " x ", "$trim": False},
            "n": 5, "yes": True,
            "first": {"$first": ["nope?", "country_id"]},
        },
    },
}
rc, _, err = build(city)
check("city template builds", rc == 0, err)
r1 = look("city.mmdb", "1.0.0.1")
check("joins: country, and place by the country's id",
      r1 and r1["country"] == {"iso_code": "US", "names": {"en": "United States", "de": "Vereinigte Staaten"},
                               "continent": "NA"}, r1)
check("a table with trim false keeps the spaces; trim true takes them off (with Unicode white space)",
      r1 and r1["city"] == {"names": {"en": "San Jose ", "de": " San José"}} and r1["country"]["iso_code"] == "US",
      r1)
check("$strip_prefix: us-ca in US is CA", r1 and r1.get("state") == "CA", r1)
check("$number and $ceil, typed by key name", r1 and r1["latitude"] == 37.5 and r1["accuracy_radius"] == 13, r1)
check("$parse literal: Python repr, \\x escapes, typed keys",
      r1 and r1["routes"] == ["1.0.0.0/24", "2001:db8::/32"]
      and r1["peers"] == [{"as_number": "13335", "name": "Cloud flare"}], r1)
check("$parse list: Perl's split, empty middle kept, trailing dropped", r1 and r1["flags"] == ["A", "B"], r1)
check("constants: a string through its steps, a number, a boolean",
      r1 and r1["fixed"] == " x " and r1["n"] == 5 and r1["yes"] is True, r1)
check("$first: an absent optional column, then the next", r1 and r1["first"] == "1", r1)
check("a uint16 from a column", r1 and r1["score"] == 7, r1)
r6 = look("city.mmdb", "2001:db8::1")
check("a prefix of another country empties the code; $empty omit leaves it out", r6 and "state" not in r6, r6)
check("an invalid number is left out; an empty one too", r6 and "latitude" not in r6 and "accuracy_radius" not in r6, r6)
check("$default fills an empty value", r6 and r6["score"] == 0, r6)
check("$parse literal of [] and list of []", r6 and r6["routes"] == [] and r6["flags"] == [], r6)
check("skip_if: ZZ and an unknown country are left out",
      look("city.mmdb", "2.0.0.1") is None and look("city.mmdb", "3.0.0.1") is None)
r4 = look("city.mmdb", "4.0.0.1")
check("$missing omit drops the map (an empty map stays, under keep); invalid values are left out (invalid omit)",
      r4 and r4["city"] == {} and "continent" in r4["country"] and "latitude" not in r4 and "accuracy_radius" not in r4
      and "score" not in r4 and r4.get("state") == "ON", r4)
check("the log: skipped rows, a table's missing ids, invalid values",
      "2 left out by the record's skip rules" in err and 'table "place" had no row for the id in 1 lookup' in err
      and "latitude: 2 invalid values left out" in err and "score: 1 invalid value left out" in err, err)

# ---- $invalid bad and skip, $missing skip and empty, record.empty and skip_empty ----
write("a.csv", "network,v,id\n10.0.0.0/24,1,1\n10.0.1.0/24,x,1\n10.0.2.0/24,2,9\n10.0.3.0/24,,1\n")
write("t1.csv", "id,name,code\n1,One,\n")
base = {"input": "a.csv", "output": "a.mmdb",
        "record": {"tables": {"t": "t1.csv"}, "types": {"v": "uint16"},
                   "shape": {"v": "v", "t": {"$table": "t", "$on": "id", "name": "name", "code": "code"}}}}
rc, _, err = build(base)
check("$invalid bad (the default): the row is skipped with a warning",
      rc == 0 and look("a.mmdb", "10.0.1.1") is None and 'v: cannot read "x" as uint16' in err, err)
check("$missing empty: every column of an unknown id reads \"\"",
      look("a.mmdb", "10.0.2.1") == {"v": 2, "t": {"name": "", "code": ""}}, look("a.mmdb", "10.0.2.1"))
cfg = json.loads(json.dumps(base))
cfg["record"]["shape"]["v"] = {"$col": "v", "$invalid": "skip"}
cfg["record"]["shape"]["t"]["$missing"] = "skip"
rc, _, err = build(cfg)
check("$invalid skip and $missing skip leave the rows out quietly",
      rc == 0 and look("a.mmdb", "10.0.1.1") is None and look("a.mmdb", "10.0.2.1") is None
      and "2 left out" in err and "cannot read" not in err, err)
cfg = json.loads(json.dumps(base))
cfg["record"]["empty"] = "omit"
cfg["record"]["skip_empty"] = True
cfg["record"]["shape"] = {"t": {"$table": "t", "$on": "id", "code": "code"}, "v": "v"}
rc, _, err = build(cfg)
check("record.empty omit drops empty values and the maps left empty",
      rc == 0 and look("a.mmdb", "10.0.0.1") == {"v": 1}, look("a.mmdb", "10.0.0.1"))
check("skip_empty leaves out rows whose record ends up empty", look("a.mmdb", "10.0.3.1") is None and "1 left out" in err,
      err)

# ---- repeated keys: an earlier row's record is reused, skip rules too ----
lines = ["network,id,flag"]
for i in range(3000):
    lines.append("10.%d.%d.0/24,%d,%s" % (i // 256, i % 256, i % 3, "ZZ" if i % 3 == 2 else "ok"))
write("rep.csv", "\n".join(lines) + "\n")
write("rep_t.csv", "id,name,long\n0,zero,%s\n1,one,%s\n" % ("x" * 300000, "y" * 5))
rep = {"input": "rep.csv", "output": "rep.mmdb",
       "record": {"tables": {"t": "rep_t.csv"}, "skip_if": [{"value": "flag", "in": ["ZZ"]}],
                  "shape": {"t": {"$table": "t", "$on": "id", "name": "name", "long": "long"}, "id": "id"}}}
rc, _, err = build(rep)
r0, r1 = look("rep.mmdb", "10.0.0.1"), look("rep.mmdb", "10.11.182.1")
check("repeated keys: every row gets its record, a skip rule counts every row",
      rc == 0 and r0 == {"t": {"name": "zero", "long": "x" * 300000}, "id": "0"}
      and r1 == {"t": {"name": "one", "long": "yyyyy"}, "id": "1"} and look("rep.mmdb", "10.0.2.1") is None
      and "1,000 left out" in err and "2 unique records" in err, err)
rc, _, err = build(dict(rep, output="rep2.mmdb", fields={"network_field": True}))
r0 = look("rep2.mmdb", "10.0.3.1")
check("with --network-field (no key) the records are built per row",
      rc == 0 and r0 and r0["network"] == "10.0.3.0/24" and r0["t"]["name"] == "zero" and "1,000 left out" in err, err)

# ---- shared values: a big table row is built once and kept once ----
write("sh_country.csv", "id,code,long\n1,AA,%s\n2,BB,short\n" % ("c" * 1500))
write("sh_asn.csv", "id,num,routes,country_id,bad\n1,1,%s,1,x\n2,2,short,2,5\n3,3,%s,2,7\n4,4,,1,9\n"
      % ("r" * 2000, "q" * 3000))
lines = ["start_ip,end_ip,city,asn_id"]
for i in range(600):
    lines.append("10.%d.%d.0,10.%d.%d.255,c%d,%d" % (i // 256, i % 256, i // 256, i % 256, i, [1, 2, 3, 9][i % 4]))
write("sh.csv", "\n".join(lines) + "\n")
sh_shape = {"city": "city",
            "asn": {"$table": "asn", "$on": "asn_id", "$missing": "omit", "num": "num", "routes": "routes",
                    "bad": {"$col": "bad", "$number": [0, 100], "$invalid": "omit"},
                    "country": {"$table": "country", "$on": "country_id", "code": "code", "long": "long"}}}
sh = {"input": "sh.csv", "output": "sh.mmdb",
      "record": {"tables": {"asn": "sh_asn.csv", "country": "sh_country.csv"}, "empty": "omit", "shape": sh_shape}}
rc, _, err = build(sh)
country = {"1": {"code": "AA", "long": "c" * 1500}, "2": {"code": "BB", "long": "short"}}
asn = {"1": {"num": "1", "routes": "r" * 2000, "country": country["1"]},
       "2": {"num": "2", "routes": "short", "bad": "5", "country": country["2"]},
       "3": {"num": "3", "routes": "q" * 3000, "bad": "7", "country": country["2"]},
       "4": {"num": "4", "bad": "9", "country": country["1"]}}
want = {"10.%d.%d.1" % (i // 256, i % 256): dict({"city": "c%d" % i}, **({"asn": asn[str([1, 2, 3, 9][i % 4])]} if i % 4 != 3 else {}))
        for i in range(0, 600, 7)}
got = {ip: look("sh.mmdb", ip) for ip in want}
check("shared values: big table rows (and a big row inside one) read back in every record",
      rc == 0 and got == want, err if rc else [(ip, got[ip], want[ip]) for ip in want if got[ip] != want[ip]][:2])
check("shared values: a value's notes count every row that has it, as without sharing",
      "asn.bad: 150 invalid values left out" in err and 'table "asn" had no row for the id in 150 lookups' in err
      and "600 unique records" in err, err)
rc, out, _ = run("verify", "--db", "sh.mmdb")
check("shared values: the database verifies", rc == 0 and "valid" in out, out)


def deep(old, new):
    if isinstance(old, dict) and isinstance(new, dict):
        return {k: deep(old[k], new[k]) if k in old and k in new else new.get(k, old.get(k)) for k in {**old, **new}}
    return new


write("sh_m.csv", "start_ip,end_ip,city,asn_id\n"
      "1.0.0.0,1.0.0.255,a,1\n1.0.0.0,1.0.0.127,b,4\n"  # big then small: a new map
      "2.0.0.0,2.0.0.255,a,3\n2.0.0.0,2.0.0.127,b,3\n"  # the same big row twice
      "3.0.0.0,3.0.0.255,a,1\n3.0.0.0,3.0.0.127,b,3\n")  # two big rows
for merge in ("recurse", "toplevel", "keep"):
    rc, _, err = build(dict(sh, input="sh_m.csv", output="sh_m.mmdb", merge=merge))
    rows = {"1": ("a", "1", "b", "4"), "2": ("a", "3", "b", "3"), "3": ("a", "1", "b", "3")}
    ok = rc == 0
    for net, (c1, a1, c2, a2) in rows.items():
        old, new = {"city": c1, "asn": asn[a1]}, {"city": c2, "asn": asn[a2]}
        w = deep(old, new) if merge == "recurse" else old if merge == "keep" else {**old, **new}
        g = look("sh_m.mmdb", net + ".0.0.1")
        ok = ok and g == w and look("sh_m.mmdb", net + ".0.0.200") == old
    check("shared values merge (%s): the overlap holds the merged record" % merge, ok, err)

# ---- order: which overlapping row is later ----
write("o.csv", "network,v\n1.0.0.0/24,four\n::/64,six\n")
o = {"input": "o.csv", "output": "o.mmdb", "record": {"shape": {"v": "v"}}}
rc, _, err = build(o)
check("order file: the later IPv6 row covers the IPv4 one", rc == 0 and look("o.mmdb", "1.0.0.1") == {"v": "six"}, err)
o["order"] = "ipv6-first"
rc, _, err = build(o)
check("order ipv6-first: IPv4 rows count as later", rc == 0 and look("o.mmdb", "1.0.0.1") == {"v": "four"}, err)
rc, _, err = run("import", "o.csv", "o2.mmdb", "--order", "ipv6-first", "-q")
check("--order works without a template", rc == 0 and look("o2.mmdb", "1.0.0.1") == {"v": "four"}, err)
rc, _, err = run("import", "o.csv", "o3.mmdb", "--order", "sideways")
check("--order takes file or ipv6-first", rc == 1 and "file or ipv6-first" in err, err)

# ---- vars, --var, the environment, and .gz inputs ----
os.makedirs("data/sub", exist_ok=True)
with gzip.open("data/sub/in.csv.gz", "wt") as f:
    f.write("network,id\n10.0.0.0/8,1\n")
with gzip.open("data/sub/tab.csv.gz", "wt") as f:
    f.write("id,name\n1,gz table\n")
v = {"vars": {"root": "nowhere"}, "input": "${root}/sub/in.csv", "output": "${OUTDIR}/v.mmdb",
     "record": {"tables": {"t": "${root}/sub/tab.csv"}, "shape": {"n": {"$table": "t", "$on": "id", "$col": "name"}}}}
env = dict(os.environ, OUTDIR=os.path.abspath("."))
config("v.json", v)
rc, _, err = run("import", "--config", "v.json", "--var", "root=" + os.path.abspath("data"), env=env)
check("--var over the config's vars, an environment variable, and inputs found as .gz",
      rc == 0 and look("v.mmdb", "10.1.1.1") == {"n": "gz table"}, err)
rc, _, err = run("import", "--config", "v.json", env=env)
check("the config's vars apply without --var", rc == 1 and "nowhere/sub/in.csv" in err, err)
v["output"] = "${NOPE_NOT_SET}/v.mmdb"
config("v.json", v)
rc, _, err = run("import", "--config", "v.json")
check("an unknown ${name} is an error", rc == 1 and "no value for ${NOPE_NOT_SET}" in err, err)
rc, _, err = run("import", "--config", "v.json", "--var", "=x")
check("--var needs name=value", rc == 1 and "--var needs name=value" in err, err)

# ---- mistakes in the template ----
write("m.csv", "network,a\n10.0.0.0/8,x\n")
mistakes = [
    ({"shape": {"x": "b"}}, 'the input m.csv has no column "b"'),
    ({"shape": {"x": {"$col": "a", "$bogus": 1}}}, 'unknown directive "$bogus"'),
    ({"shape": {"x": {"$on": "a", "$col": "a"}}}, "$on goes with $table"),
    ({"shape": {"x": {"$table": "t", "$on": "a", "$col": "a"}}}, 'no table "t"'),
    ({"shape": {"x": {"$col": "a", "$const": "y"}}}, "holds more than one of"),
    ({"shape": {"x": {"k": "a", "$case": "upper"}}}, "$case applies to $col"),
    ({"shape": {"x": {"$use": "nope"}}}, "$use names no entry"),
    ({"define": {"d": {"$use": "d"}}, "shape": {"x": {"$use": "d"}}}, "too deeply"),
    ({"shape": {"x": {"$col": "a", "$number": [5, 1]}}}, "must be [min, max]"),
    ({"shape": {"x": "a"}, "skip_if": [{"value": "a"}]}, '"value": ..., "in"'),
    ({"shape": ["a"]}, "the record is a map"),
    ({}, 'needs "shape"'),
    ({"shape": {"x": "a"}, "extra": 1}, 'unknown key "extra"'),
]
for rec, want in mistakes:
    rc, _, err = build({"input": "m.csv", "output": "m.mmdb", "record": rec}, name="m.json")
    check("template mistake: " + want, rc == 1 and want in err and "m.json:" in err, err)
rc, _, err = build({"input": "m.csv", "output": "m.mmdb", "record": {"shape": {"x": "a"}},
                    "fields": {"include": ["a"]}}, name="m.json")
check("record and fields.include do not go together", rc == 1 and "fields.include" in err, err)
write("m.json.in", json.dumps({"input": "x.json", "output": "m.mmdb", "record": {"shape": {"x": "a"}}}))
write("x.json", '[{"network": "10.0.0.0/8", "a": "x"}]')
rc, _, err = run("import", "--config", "m.json.in")
check("a template reads CSV, not JSON", rc == 1 and "reads CSV or TSV input" in err, err)
rc, _, err = build({"input": "m.csv", "output": "m.mmdb",
                    "record": {"tables": {"t": "missing.csv"}, "shape": {"x": "a"}}}, name="m.json")
check("a missing table file is an error", rc == 1 and 'table "t"' in err and "missing.csv" in err, err)

print("----")
print("%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
