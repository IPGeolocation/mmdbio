"""The features beyond the Go mmdbio: arguments instead of flags, read and
export as JSON Lines, CSV and TSV, metadata's layout and data types, diff by
lookup, the new import options, completion install, shrink's arguments.

    python3 check_features.py path/to/mmdbio workdir
"""
import csv
import io
import json
import os
import shutil
import subprocess
import sys

M = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
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


def run(*args, stdin=None, env=None):
    p = subprocess.run([M, *args], input=stdin, capture_output=True, env=env)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def write(path, text):
    with open(path, "w", newline="") as f:
        f.write(text)


shutil.copy(os.path.join(here, "data", "city_data.json"), ".")
rc, _, err = run("import", "city_data.json", "city.mmdb", "--ip", "4", "-q")
check("import with arguments", rc == 0 and os.path.exists("city.mmdb"), err)

# ---- read ----
_, by_flag, _ = run("read", "--db", "city.mmdb", "--ip", "8.8.8.8")
rc, by_arg, _ = run("read", "8.8.8.8", "city.mmdb")
check("read IP db.mmdb prints what --ip does", rc == 0 and by_arg == by_flag and "Mountain View" in by_arg)
write("ips.txt", "1.1.1.1\n# a comment\nbogus\n9.9.9.9-9.9.9.9\n")
rc, out, err = run("read", "8.8.8.0/31", "1.1.1.0,1.1.1.1", "ips.txt", "city.mmdb", "-f", "jsonl",
                   "--fields", "location.city.name")
rows = [json.loads(line) for line in out.splitlines()]
check("read: CIDRs, ranges and files in jsonl",
      rc == 0 and [r["ip"] for r in rows] == ["8.8.8.0", "8.8.8.1", "1.1.1.0", "1.1.1.1", "1.1.1.1", "9.9.9.9"]
      and rows[0] == {"ip": "8.8.8.0", "location.city.name": "Mountain View"} and "invalid IP address bogus" in err,
      (out, err))
rc, out, _ = run("read", "8.8.8.8", "1.1.1.1", "7.7.7.7", "city.mmdb", "-f", "csv", "--flatten")
table = list(csv.reader(io.StringIO(out)))
check("read -f csv --flatten: a header, one row per IP with data",
      rc == 0 and table[0][:3] == ["ip", "location.city.name", "location.continent.name"] and len(table) == 3
      and table[1][1] == "Mountain View" and table[2][0] == "1.1.1.1", out)
rc, out, _ = run("read", "8.8.8.8", "city.mmdb", "-f", "csv")
table = list(csv.reader(io.StringIO(out)))
check("read -f csv: a nested map is JSON in one cell",
      rc == 0 and table[0] == ["ip", "location"] and json.loads(table[1][1])["city"]["name"] == "Mountain View", out)
rc, out, _ = run("read", "-", "city.mmdb", "-f", "tsv", "--no-header", "--fields",
                 "location.country.iso_code,location.latitude", stdin=b"8.8.8.8\n9.9.9.9\n")
check("read - (stdin) -f tsv --no-header", rc == 0 and out == "8.8.8.8\tUS\t37.4056\n9.9.9.9\tCH\t47.3769\n", out)
rc, out, _ = run("read", "9.9.9.9", "city.mmdb", "-f", "json-pretty", "--fields", "location.city.name")
check("read -f json-pretty", rc == 0 and out == '{\n  "ip": "9.9.9.9",\n  "location.city.name": "Zurich"\n}\n', out)
rc, out, _ = run("read", "8.8.8.8", "7.7.7.7", "city.mmdb", "--fields", "location.city.name")
check("read: two IPs in json are keyed by IP",
      rc == 0 and json.loads(out) == {"7.7.7.7": None, "8.8.8.8": {"location.city.name": "Mountain View"}}, out)
rc, _, err = run("read", "::/64", "city.mmdb", "-f", "csv")
check("read: a range over 2^32 addresses is refused", rc == 1 and "4,294,967,296" in err, err)
rc, _, err = run("read", "nope", "city.mmdb")
check("read: not an IP, range or file", rc == 1 and "Not an IP, CIDR, range or file: nope" in err, err)
rc, out, _ = run("read", "8.8.8.8", "city.mmdb", "-f", "jsonl", "--out", "r.jsonl")
check("read --out with a format", rc == 0 and "Results saved to r.jsonl" in out and
      json.loads(open("r.jsonl").read())["ip"] == "8.8.8.8", out)

# ---- export ----
rc, out, _ = run("export", "city.mmdb", "e.csv")
table = list(csv.reader(open("e.csv", newline="")))
check("export db out.csv: CSV from the extension", rc == 0 and table[0] == ["range", "location"] and
      [r[0] for r in table[1:]] == ["1.1.1.0/24", "8.8.8.0/24", "9.9.9.0/24"], out)
rc, out, _ = run("export", "city.mmdb", "e.jsonl")
lines = [json.loads(x) for x in open("e.jsonl")]
check("export to .jsonl", rc == 0 and lines[1]["range"] == "8.8.8.0/24" and
      lines[1]["location"]["country"]["iso_code"] == "US", out)
rc, out, err = run("export", "city.mmdb", "-f", "tsv", "--flatten", "--fields", "location.city.name,location.latitude",
                   "--where", "location.latitude>0")
check("export -f tsv --flatten --where to stdout",
      rc == 0 and out == "range\tlocation.city.name\tlocation.latitude\n8.8.8.0/24\tMountain View\t37.4056\n"
      "9.9.9.0/24\tZurich\t47.3769\n" and "Exported 2 records to stdout" in err, (out, err))
rc, out, _ = run("export", "--db", "city.mmdb", "--out", "o.json")
check("export --format json is unchanged", rc == 0 and open("o.json").read() ==
      open(os.path.join(here, "data", "export.json")).read())
# round trips through import
run("export", "city.mmdb", "flat.csv", "--flatten")
rc, _, err = run("import", "flat.csv", "flat.mmdb", "--ip", "4", "--nest-keys", "--type", "location.latitude=double",
                 "--type", "location.longitude=double", "-q")
run("export", "flat.mmdb", "flat.json")
check("export --flatten CSV, import --nest-keys: the same records",
      rc == 0 and open("flat.json").read() == open("o.json").read(), err)
rc, _, err = run("import", "e.jsonl", "jl.mmdb", "--ip", "4", "-q")
run("export", "jl.mmdb", "jl.json")
check("export jsonl, import it back: the same records", rc == 0 and open("jl.json").read() == open("o.json").read(), err)
write("q.csv", "network,note\n10.0.0.0/8,\"a, \"\"quoted\"\" value\nwith a line break\"\n11.0.0.0/8,\ttab\n")
run("import", "q.csv", "q.mmdb", "-q")
for fmt in ("csv", "tsv"):
    run("export", "q.mmdb", "q2." + fmt)
    run("import", "q2." + fmt, "q2.mmdb", "-q")
    _, a, _ = run("export", "q.mmdb")
    _, b, _ = run("export", "q2.mmdb")
    check(f"export -f {fmt} quotes what needs it; import reads it back", a == b and "quoted" in a, (a, b))

# ---- metadata, stats, inspect, verify with the db as an argument ----
rc, out, _ = run("metadata", "city.mmdb", "--json", "--data-types")
m = json.loads(out) if rc == 0 else {}
types = m.get("data_types", {})
check("metadata --json --data-types: every key, the layout, sizes that add up",
      m.get("node_count") == 59 and m.get("layout", {}).get("tree_size") == 59 * 6 and
      sum(t["bytes"] for t in types.values()) == m["layout"]["data_section_size"] and
      m["layout"]["file_size"] == os.path.getsize("city.mmdb"), out)
rc, out, _ = run("metadata", "city.mmdb", "--layout")
check("metadata --layout", rc == 0 and "Format:        2.0" in out and "Data Section:" in out, out)
rc, pretty, _ = run("metadata", "city.mmdb", "-f", "pretty")
_, text, _ = run("metadata", "city.mmdb", "-f", "text")
check("metadata -f pretty is -f text", rc == 0 and pretty == text and "MMDB Metadata" in text,
      pretty)
for cmd in ("stats", "verify", "inspect"):
    rc, out, _ = run(cmd, "city.mmdb")
    check(f"{cmd} db.mmdb", rc == 0 and out.strip(), out)

# ---- diff by lookup ----
write("old.csv", "network,v\n1.0.0.0/24,a\n2.0.0.0/16,b\n3.0.0.0/24,c\n4.0.0.0/24,d\n")
write("new.csv", "network,v\n1.0.0.0/25,a\n1.0.0.128/25,a2\n2.0.0.0/16,B\n3.0.0.0/24,c\n5.0.0.0/24,e\n")
run("import", "old.csv", "old.mmdb", "--ip", "4", "-q")
run("import", "new.csv", "new.mmdb", "--ip", "4", "-q")
rc, out, _ = run("diff", "old.mmdb", "new.mmdb", "--subnets", "--records")
check("diff --subnets --records", rc == 0 and out == (
    "Subnets changed: 4\n"
    "   1.0.0.0/24 -> 1.0.0.0/25\n"
    "   1.0.0.0/24 -> 1.0.0.128/25\n"
    "   4.0.0.0/24 -> 4.0.0.0/8 (no data)\n"
    "   5.0.0.0/8 (no data) -> 5.0.0.0/24\n"
    "\nRecords changed: 1\n"
    "   2.0.0.0/16\n"
    "      - {\"v\":\"b\"}\n"
    "      + {\"v\":\"B\"}\n"), out)
rc, out, _ = run("diff", "old.mmdb", "new.mmdb", "-s", "-r", "--json")
j = json.loads(out) if rc == 0 else {}
check("diff -s -r --json", j.get("records", {}).get("2.0.0.0/16") == {"new": {"v": "B"}, "old": {"v": "b"}} and
      len(j.get("subnets", [])) == 4 and j["subnets"][2] == {"new": "4.0.0.0/8", "new_data": False,
                                                            "old": "4.0.0.0/24", "old_data": True}, out)
rc, out, _ = run("diff", "old.mmdb", "new.mmdb", "-s", "-r", "--summary")
check("diff -s -r --summary", out == "Subnets changed: 4 | Records changed: 1\n", out)
rc, out, _ = run("diff", "old.mmdb", "new.mmdb", "--summary")
check("diff old new (arguments, the exact-prefix compare)", out == "Added: 3 | Removed: 2 | Modified: 1\n", out)
# {"v": "b"} with a pointer past the data section for its value
raw = open("old.mmdb", "rb").read()
with open("bad.mmdb", "wb") as f:
    f.write(raw.replace(b"\xe1\x41v\x41b", b"\xe1\x41v\x27\xff"))
rc, out, err = run("diff", "bad.mmdb", "new.mmdb", "--summary")
check("diff: a record that does not decode stops it with a warning and exit status 1",
      raw.count(b"\xe1\x41v\x41b") == 1 and rc == 1 and "error reading old db" in err, (rc, out, err))

# ---- import ----
write("nf.csv", "net,kind\n1.0.0.0/24,vpn\n2.0.0.0-2.0.0.9,tor\n3.3.3.3,proxy\n")
run("import", "nf.csv", "nf.mmdb", "--network-field", "-q")
_, out, _ = run("read", "1.0.0.1", "2.0.0.5", "3.3.3.3", "nf.mmdb", "-f", "jsonl")
check("import --network-field: the row's CIDR, or its range", [json.loads(x)["network"] for x in out.splitlines()] ==
      ["1.0.0.0/24", "2.0.0.0-2.0.0.9", "3.3.3.3/32"], out)
rc, _, err = run("import", "-", "--columns", "ip", "--no-fields", "--out", "-", "-q", stdin=b"10.0.0.1\n10.0.0.2\n")
check("import - --out -: stdin to stdout", rc == 0 and "to stdout" in err, err)
p = subprocess.run([M, "import", "-", "--columns", "ip", "--no-fields", "--out", "-", "-q"], input=b"10.0.0.1\n",
                   capture_output=True)
with open("bl.mmdb", "wb") as f:
    f.write(p.stdout)
_, out, _ = run("read", "10.0.0.1", "10.0.0.3", "bl.mmdb", "-f", "jsonl")
check("import --no-fields: empty records, a membership list", out == '{"ip":"10.0.0.1"}\n', out)
write("hl.csv", "5.5.5.0/24,DE,hosting\n6.6.6.0/24,US,isp\n")
run("import", "hl.csv", "hl.mmdb", "--columns", "network,country,type", "-q")
_, out, _ = run("read", "5.5.5.5", "hl.mmdb", "-f", "jsonl")
check("import --columns names a headerless CSV", json.loads(out) == {"country": "DE", "ip": "5.5.5.5", "type": "hosting"},
      out)
for merge in ("none", "keep"):
    run("import", "hl.csv", "fg.mmdb", "--columns", "network,country,type", "--fill-gaps", "--merge", merge, "--ip", "4",
        "-q")
    _, out, _ = run("read", "5.5.5.5", "7.7.7.7", "fg.mmdb", "-f", "jsonl")
    got = [json.loads(x) for x in out.splitlines()]
    check(f"import --fill-gaps --merge {merge}: rows win, gaps get \"\"",
          got == [{"country": "DE", "ip": "5.5.5.5", "type": "hosting"}, {"country": "", "ip": "7.7.7.7", "type": ""}],
          out)
write("fg.jsonl", '{"network":"8.8.8.0/24","a":"x"}\n{"network":"9.9.9.0/24","b":"y"}\n')
run("import", "fg.jsonl", "fgt.mmdb", "--fill-gaps", "--merge", "toplevel", "--ip", "4", "-q")
_, out, _ = run("read", "8.8.8.8", "1.1.1.1", "fgt.mmdb", "-f", "jsonl")
check("import --fill-gaps --merge toplevel: every field everywhere",
      [json.loads(x) for x in out.splitlines()] == [{"a": "x", "b": "", "ip": "8.8.8.8"}, {"a": "", "b": "", "ip": "1.1.1.1"}],
      out)
lines = ["network,kind"] + [f"10.{i}.{j}.0/24,{('a', 'b')[(i + j) % 2]}" for i in range(40) for j in range(40)]
write("many.csv", "\n".join(lines) + "\n")
run("import", "many.csv", "many.mmdb", "-q")
rc, _, err = run("import", "many.csv", "many-small.mmdb", "--shrink", "-q")
_, d, _ = run("diff", "many.mmdb", "many-small.mmdb", "-s", "-r", "--summary")
check("import --shrink: smaller, checked, the same lookups",
      rc == 0 and "checked" in err and os.path.getsize("many-small.mmdb") < os.path.getsize("many.mmdb") and
      d == "Subnets changed: 0 | Records changed: 0\n", (err, d))
cfg = {"input": {"path": "hl.csv", "columns": ["network", "country", "type"]}, "output": "cfg.mmdb",
       "fields": {"network_field": True}, "tree": {"fill_gaps": True}, "shrink": True, "metadata": {"ip_version": 4}}
write("cfg.json", json.dumps(cfg))
rc, _, err = run("import", "--config", "cfg.json", "-q")
_, out, _ = run("read", "5.5.5.5", "7.7.7.7", "cfg.mmdb", "-f", "jsonl")
check("the config keys: input.columns, fields.network_field, tree.fill_gaps, shrink",
      rc == 0 and [json.loads(x) for x in out.splitlines()] ==
      [{"country": "DE", "ip": "5.5.5.5", "network": "5.5.5.0/24", "type": "hosting"},
       {"country": "", "ip": "7.7.7.7", "type": ""}], (err, out))

# ---- completion install, shrink's arguments, --nocolor ----
home = os.path.join(work, "home")
os.makedirs(home)
write(os.path.join(home, ".bashrc"), "export X=1\n")
env = dict(os.environ, HOME=home, XDG_DATA_HOME="", XDG_CONFIG_HOME="", ZDOTDIR="")
rc1, out1, _ = run("completion", "install", "bash", env=env)
if rc1 == 0 and "wrote " + home not in out1.replace("\\", "/"):
    # wine hands Windows programs a home of their own, whatever $HOME says
    print("SKIP  completion install: the program does not see this $HOME (wine)")
else:
    rc2, _, _ = run("completion", "install", "bash", env=env)
    rc_text = open(os.path.join(home, ".bashrc")).read()
    script = os.path.join(home, ".local", "share", "mmdbio", "completion.bash")
    check("completion install: the script, and one block in .bashrc however often",
          rc1 == rc2 == 0 and os.path.exists(script) and rc_text.count("# >>> mmdbio completion >>>") == 1, rc_text)
    rc, _, _ = run("completion", "install", "fish", env=env)
    check("completion install fish", rc == 0 and os.path.exists(os.path.join(home, ".config", "fish", "completions",
                                                                             "mmdbio.fish")))
    rc, out, _ = run("completion", "uninstall", "bash", env=env)
    check("completion uninstall", rc == 0 and open(os.path.join(home, ".bashrc")).read() == "export X=1\n" and
          not os.path.exists(script), out)
rc, out, _ = run("shrink", "many.mmdb", "ms.mmdb", "--lookups", "1000", "--bench=false")
check("shrink in out", rc == 0 and "[OK] random IPv4" in out and os.path.exists("ms.mmdb"), out)
rc, out, _ = run("shrink", "--help")
check("shrink --help", rc == 0 and "Usage:\n  mmdbio shrink " in out, out)
rc, out, _ = run("__complete", "2", "shrink", "--record-size", "")
check("shrink's flag values complete", out.split() == ["0", "24", "28", "32"], out)
rc, out, _ = run("--nocolor", "diff", "old.mmdb", "new.mmdb", "-s")
check("--nocolor anywhere", rc == 0 and "\033[" not in out and "Subnets changed: 4" in out, out)
rc, out, _ = run("--help")
check("--help lists shrink", rc == 0 and "shrink " in out and "--nocolor" in out, out)

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
