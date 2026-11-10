#!/bin/sh
# End-to-end tests: run_tests.sh path/to/mmdbio
#
# 1. The documentation's sample database through every read command, with
#    the outputs the Go mmdbio documents.
# 2. The command line: help, version, errors, completion.
# 3. check_build.py: import against a brute-force model.
# 4. check_robust.py: hostile inputs fail cleanly.
# 5. check_shrink.py: shrink on hand-built files, read back independently.
# 6. check_features.py: everything beyond the Go mmdbio.
# 7. check_edges.py: databases built with every feature, checked with an
#    independent reader, then read, exported, re-imported and shrunk.
# 8. check_template.py: record templates (tables, nested joins, value steps),
#    "order", "vars" and --var, read back independently.
# $PYTHON names the Python 3 to use (default python3).
set -u
PY=${PYTHON:-python3}
here=$(cd "$(dirname "$0")" && pwd)
M=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=${TMPDIR:-/tmp}/mmdbio-tests.$$
rm -rf "$work"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
pass=0
fail=0
ok() { pass=$((pass + 1)); printf 'PASS  %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/      /' | head -20; }
same() { # name, expected stdout, command...
	name=$1 want=$2
	shift 2
	got=$("$@" 2>/dev/null)
	if [ "$got" = "$want" ]; then ok "$name"; else bad "$name" "$(printf 'got:\n%s\nwant:\n%s' "$got" "$want")"; fi
}

cd "$work" || exit 1
cp "$here/data/city_data.json" .
"$M" import --in city_data.json --out ip-to-city.mmdb --ip 4 --size 32 --title Demo-City-DB \
	--description "Demo IP to City database" -q > /dev/null 2>&1 || bad "build the sample database"

same "read --ip" "$(cat "$here/data/read-8.8.8.8.json")" "$M" read --db ip-to-city.mmdb --ip 8.8.8.8
same "read --fields" '{
  "8.8.8.8": {
    "location.city.name": "Mountain View",
    "location.country.name": "United States"
  }
}' "$M" read --db ip-to-city.mmdb --ip 8.8.8.8 --fields location.country.name,location.city.name
same "read: no data" "No data found for that IP." "$M" read --db ip-to-city.mmdb --ip 192.168.0.1
same "read --range" '{
  "7.7.7.0": null,
  "7.7.7.1": null
}' "$M" read --db ip-to-city.mmdb --range 7.7.7.0/31
same "read --range --fields" '{
  "8.8.8.0": {
    "nope": null
  },
  "8.8.8.1": {
    "nope": null
  }
}' "$M" read --db ip-to-city.mmdb --range 8.8.8.0/31 --fields nope
printf '8.8.8.8\n\n  bogus  \n1.1.1.1\n' > ips.txt
got=$("$M" read --db ip-to-city.mmdb --input ips.txt --fields location.city.name)
case "$got" in *'"1.1.1.1": {'*'"Sydney"'*'"bogus": "invalid_ip"'*) ok "read --input" ;; *) bad "read --input" "$got" ;; esac
"$M" read --db ip-to-city.mmdb --input ips.txt --out res.json > out.txt
if grep -q "Results saved to res.json" out.txt && grep -q Sydney res.json && [ "$(tail -c 1 res.json)" = "}" ]; then
	ok "read --out"
else bad "read --out" "$(cat out.txt)"; fi
got=$("$M" metadata --db ip-to-city.mmdb | grep -v "Build Date")
same "metadata" "$(grep -v "Build Date" "$here/data/metadata.txt")" printf '%s' "$got"
got=$("$M" stats --db ip-to-city.mmdb | grep -v "Build Epoch")
same "stats" "$(grep -v "Build Epoch" "$here/data/stats.txt")" printf '%s' "$got"
got=$("$M" stats --db ip-to-city.mmdb --json | grep -v build_epoch)
same "stats --json" "$(grep -v build_epoch "$here/data/stats.json")" printf '%s' "$got"
same "inspect" "$(cat "$here/data/inspect.txt")" "$M" inspect --db ip-to-city.mmdb --sample-ip 8.8.8.8
same "inspect: sample not found" "No record found for this IP." "$M" inspect --db ip-to-city.mmdb
"$M" inspect --db ip-to-city.mmdb --sample-ip 8.8.8.8 --out schema/dir/schema.json > /dev/null
same "inspect --out" "$(cat "$here/data/schema.json")" cat schema/dir/schema.json
same "verify" "valid" "$M" verify --db ip-to-city.mmdb
printf 'not an mmdb' > junk.mmdb
same "verify: invalid" "invalid: error opening database: invalid MaxMind DB file" "$M" verify --db junk.mmdb
"$M" export --db ip-to-city.mmdb --out output.json > out.txt
same "export" "$(cat "$here/data/export.json")" cat output.json
same "export: summary" "Exported 3 records to output.json" cat out.txt
same "export --where --format cidr" "8.8.8.0/24
9.9.9.0/24" "$M" export --db ip-to-city.mmdb --where 'location.country.iso_code=us,CH' --format cidr --out -
same "export --where numeric" "1.1.1.0/24" "$M" export --db ip-to-city.mmdb --where 'location.latitude<0' --format cidr \
	--out - 2>/dev/null
same "export --range" "8.8.8.0/24" "$M" export --db ip-to-city.mmdb --range 8.0.0.0/8 --format cidr --out - 2>/dev/null
same "export --family 6" "" "$M" export --db ip-to-city.mmdb --family 6 --format cidr --out - 2>/dev/null
same "diff: identical" "Added: 0 | Removed: 0 | Modified: 0" "$M" diff --old ip-to-city.mmdb --new ip-to-city.mmdb --summary

# 2. the command line
case "$("$M" --version)" in "mmdbio version "*) ok "--version" ;; *) bad "--version" ;; esac
case "$("$M" --help)" in *"Available Commands:"*import*verify*) ok "--help lists the commands" ;; *) bad "--help" ;; esac
case "$("$M" help export)" in *"--where stringArray"*) ok "help export" ;; *) bad "help export" ;; esac
"$M" read > out.txt 2>&1
rc=$?
if [ $rc = 1 ] && grep -q "Error: --db flag is required" out.txt; then ok "read without --db"; else bad "read without --db" "$(cat out.txt)"; fi
"$M" read --db ip-to-city.mmdb --nope > out.txt 2>&1
rc=$?
if [ $rc = 1 ] && grep -q "unknown flag: --nope" out.txt; then ok "unknown flag"; else bad "unknown flag" "$(cat out.txt)"; fi
"$M" nosuch > out.txt 2>&1
if [ $? = 1 ] && grep -q 'unknown command "nosuch"' out.txt; then ok "unknown command"; else bad "unknown command"; fi
"$M" read --db missing.mmdb --ip 1.1.1.1 > out.txt 2>&1
if [ $? = 1 ] && grep -q "Failed to open MMDB file: open missing.mmdb: no such file or directory" out.txt; then
	ok "a missing file"
else bad "a missing file" "$(cat out.txt)"; fi
for sh in bash zsh fish powershell; do
	if "$M" completion "$sh" | grep -q __complete; then ok "completion $sh"; else bad "completion $sh"; fi
done
if ! command -v bash > /dev/null; then
	ok "the bash completion parses (no bash here: not checked)"
elif bash -n -c "$("$M" completion bash)"; then ok "the bash completion parses"; else bad "bash completion"; fi
got=$("$M" __complete 2 export --format "" | tr '\n' ' ')
if [ "$got" = "json jsonl csv tsv cidr " ]; then ok "completion: a flag's values"; else bad "completion values" "$got"; fi
got=$("$M" __complete 1 verify "" | tr '\n' ' ')
if [ "$got" = "__files__ .mmdb " ]; then ok "completion: .mmdb files for a database"; else bad "completion files" "$got"; fi
"$M" completion > out.txt 2>&1
if [ $? = 1 ]; then ok "completion needs a shell"; else bad "completion needs a shell"; fi
"$M" import > out.txt 2>&1
if [ $? = 1 ] && grep -q "in is required" out.txt; then ok "import without --in"; else bad "import without --in" "$(cat out.txt)"; fi
printf 'network,v\n10.0.0.0/8,x\n' > data.csv
"$M" import --in data.csv -q > import.txt 2>&1
"$M" metadata --db data.mmdb > meta.txt 2>&1
if [ -f data.mmdb ] && grep -q "Database Type: data" meta.txt; then
	ok "import: the output and title default to the input's name"
else bad "import defaults" "$(cat import.txt meta.txt; ls -l)"; fi

# 3 to 8
for check in check_build check_robust check_shrink check_features check_edges check_template; do
	if $PY -I "$here/$check.py" "$M" "$work/$check" > "$check.log" 2>&1; then
		ok "$check.py ($(tail -1 "$check.log"))"
	else bad "$check.py" "$(grep -E '^FAIL|Error|Traceback' -A5 "$check.log" | head -30)"; fi
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
