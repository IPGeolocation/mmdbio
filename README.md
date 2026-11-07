# mmdbio: an MMDB command-line toolkit, in C

`mmdbio` reads and writes MMDB files, the compact binary format used for
offline IP geolocation and IP intelligence lookups. It can:

- look up IPs one at a time, from lists and files, or across CIDRs and
  ranges, as JSON, JSON Lines, CSV or TSV;
- export records to JSON, JSON Lines, CSV or TSV, or the networks that match
  a filter to a CIDR list for firewalls and WAFs;
- compare two files, validate one, inspect its schema and its layout;
- **shrink a file losslessly**, storing identical parts of its search tree
  once (checked against the original before it is kept);
- **build MMDB files from CSV, TSV, JSON or NDJSON, gzipped or not**, with or
  without a config file.

It is a C rewrite of the Go `mmdbio` 1.2 with the same commands and flags, so
existing scripts keep working. Every command runs offline.

- **One static binary.** zlib is bundled, so there are no runtime
  dependencies and the build needs only a C compiler.
- **Low memory.** The builder streams the search tree to disk instead of
  holding it in memory. A 2-million-row gzipped CSV builds in about 6
  seconds with a 240 MB peak.
- **Constant-memory reads.** `export` and `diff` stream through the files,
  so a database of any size exports without being loaded.
- **Everywhere.** Linux (amd64, arm64, armv7, 386), macOS (arm64, amd64),
  Windows (amd64, 386), FreeBSD, and the other BSDs and illumos from source.
- **A C library** too: `libmmdbio` does all of this for C programs.

## Install

| system | how |
| --- | --- |
| Linux, macOS | `curl -fsSL https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.sh \| sh` |
| Windows (PowerShell) | `irm https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.ps1 \| iex` |
| Debian, Ubuntu | `sudo apt install ./mmdbio_<version>_amd64.deb` from a release |
| Fedora, RHEL, SUSE | `sudo rpm -i mmdbio-<version>-1.x86_64.rpm` from a release |
| Homebrew | the formula in `packaging/homebrew/mmdbio.rb` |
| anything else | build from source (below) |

The install scripts download the release archive for your system, check it
against the release's `SHA256SUMS`, and install `mmdbio` into
`/usr/local/bin` (or `~/.local/bin`; on Windows
`%LOCALAPPDATA%\Programs\mmdbio`, added to your PATH). `MMDBIO_VERSION`
picks a version, `MMDBIO_DIR` the directory.

Release builds: Linux amd64, arm64, armv7 and 386 (fully static), macOS
arm64 and amd64, Windows amd64 and 386, FreeBSD, OpenBSD, NetBSD and
DragonFly BSD (amd64). Each archive also holds the library (`libmmdbio.a`)
and its header (`mmdbio.h`).

### Build from source

You need a C compiler and make; there is nothing else to install.

```bash
git clone <this repository> mmdbio-c
cd mmdbio-c
make
sudo make install      # mmdbio in /usr/local/bin, the library and its header
```

`make test` runs the test suite (it needs `python3`). On Linux the binary is
fully static, so it runs on any Linux machine of the same architecture.

| target | command |
| --- | --- |
| this machine | `make` (`gmake` on the BSDs and illumos) |
| Windows, from Linux | `make CC=x86_64-w64-mingw32-gcc BUILD=build-windows` |
| 32-bit Windows, from Linux | `make CC="clang --target=i686-w64-mingw32" BUILD=build-windows-386 LIBAR=i686-w64-mingw32-ar NM=i686-w64-mingw32-nm OBJCOPY=i686-w64-mingw32-objcopy` |
| 32-bit x86 Linux | `make CC=i686-linux-gnu-gcc BUILD=build-386`, or `CC="gcc -m32"` with gcc-multilib (GCC 14 or newer, for C23's `_BitInt`; on Debian and Ubuntu gcc-multilib can't be installed beside the ARM cross compilers, gcc-i686-linux-gnu can) |
| 64-bit ARM Linux, from x86 | `make CC=aarch64-linux-gnu-gcc BUILD=build-arm64` |
| 32-bit ARM Linux, from x86 | `make CC="clang --target=armv7-linux-gnueabihf" BUILD=build-armv7` (GCC has no `_BitInt` there) |
| the library | `make lib` (`build/libmmdbio.a`; see [The library](#the-library)) |

FreeBSD, OpenBSD, NetBSD, DragonFly BSD and illumos build from source with
`gmake`, and `.github/workflows/ci.yml` builds and tests each of them. A
32-bit build maps the whole database into its address space, so it can't open
files bigger than 2 to 3 GB.

### Release files

`./build-release.sh` builds and tests, into `dist/`:

- `mmdbio-<version>-<os>-<arch>.tar.gz` (`.zip` for Windows) with `mmdbio`,
  the library and its header, the README and the license;
- on Linux, a `.deb` and an `.rpm` (with shell completions), when
  `dpkg-deb` and `rpmbuild` are installed;
- on Linux amd64, also linux-386, linux-arm64, linux-armv7, windows-amd64
  and windows-386, each tested under qemu or wine when they are installed
  (the compilers each one needs are listed at the top of the script);
- `SHA256SUMS`.

`.github/workflows/release.yml` runs it on every platform when a `v<version>`
tag is pushed and publishes the files. The `.deb`'s maintainer is
`$DEB_MAINTAINER` (default: the git user).

## Quick start

```bash
# Build a database from a CSV: only the input is required
mmdbio import ip-ranges.csv.gz

# Look up IPs: one, or any mix of IPs, CIDRs, ranges and files
mmdbio read 8.8.8.8 ip-ranges.mmdb
mmdbio read 8.8.8.0/30 1.1.1.1-1.1.1.9 ips.txt ip-ranges.mmdb -f csv

# Export to CSV (the format comes from the extension)
mmdbio export ip-ranges.mmdb ip-ranges.csv

# Export the networks of German VPN exits as a CIDR list
mmdbio export --db ip-security.mmdb --where is_vpn=true --where location.country.code2=DE --format cidr --out vpn-de.txt

# Make it smaller without changing a single lookup
mmdbio shrink ip-ranges.mmdb
```

Every command also takes its files as flags (`--db`, `--in`, `--out`,
`--old`, `--new`), as the Go mmdbio did.

## Commands

| command | what it does |
| --- | --- |
| `read` | look up IPs; see below |
| `metadata` | the metadata: type, IP version, record size, node count, build date, languages, description; `--layout` adds the format version and each section's size and offset, `--data-types` the data section broken down by value type, `--json` (or `-f json`; `-f text`, or mmdbctl's `-f pretty`, is the default) all of it (every metadata key) as JSON |
| `stats` | the metadata plus IPv4, IPv6 and network counts (`--json` for JSON) |
| `inspect` | the field paths and types of the record at `--sample-ip`; `--out` saves them as a JSON schema |
| `verify` | checks the tree, the data section and the metadata; prints `valid` (exit 0) or `invalid: reason` (exit 1) |
| `export` | records to JSON, JSON Lines, CSV or TSV, or networks to a CIDR list; see below |
| `import` | builds an MMDB from CSV, TSV, JSON or NDJSON; see below |
| `shrink` | a smaller file with the same lookups; see below |
| `diff` | networks added, removed or modified between two files (`--summary`, `--json`); `--subnets` and `--records` compare by lookup (see below) |
| `completion` | shell completion for bash, zsh, fish or powershell, printed, or set up by `completion install` (and removed by `completion uninstall`). The scripts ask mmdbio itself what fits: commands, flags, the values a flag takes (`--format`, `--merge`, `--record-size`, `--type field=...`), `.mmdb` files where a database goes, other files elsewhere |

The database is `--db` or an argument (the first one; for `read`, the last).
`mmdbio <command> --help` lists every flag. `--nocolor` (anywhere) or
`NO_COLOR=1` turns colours off; they only ever go to a terminal.

### read

```bash
mmdbio read 8.8.8.8 city.mmdb                       # one IP: its record
mmdbio read 8.8.8.0/24 1.1.1.0,1.1.1.9 city.mmdb    # every address of each
mmdbio read ips.txt - city.mmdb -f jsonl            # a file, and stdin
mmdbio read --db city.mmdb --ip 8.8.8.8             # the Go mmdbio's flags
```

The addresses come as arguments, in any mix: IPs, CIDRs, ranges (`a-b` or
`a,b`), files with one of those per line (`#` starts a comment), and `-` for
stdin. `--ip`, `--range` and `--input` still work. A CIDR or range is looked
up address by address, at most 2^32 of them each.

| `--format` (`-f`) | output |
| --- | --- |
| `json` (default) | one object keyed by IP, `null` where there is no data; a single IP prints its record alone |
| `jsonl` | one JSON object per IP with data, its address under `"ip"` (`json-compact` and `ndjson` work too) |
| `json-pretty` | the same objects, indented |
| `csv`, `tsv` | a header (`ip`, then the fields), then a row per IP with data |

- `--fields` picks dotted paths, in every format.
- CSV and TSV columns are every top-level field of the records looked up,
  sorted; a nested value is JSON in its cell. `--flatten` makes a column of
  every leaf instead (`location.city.name`), and `--no-header` drops the
  header.
- `--out` writes to a file.

### export

```bash
mmdbio export city.mmdb city.csv                     # CSV, from the extension
mmdbio export city.mmdb -f jsonl --where location.country.iso_code=DE
mmdbio export city.mmdb city.tsv --flatten
```

| flag | meaning |
| --- | --- |
| `--db`, `--out` | the database and the output file (or the two arguments); `-` or no output is stdout, and the summary then goes to stderr |
| `--format` (`-f`) | `json` (one object keyed by network), `jsonl` (a line per network, with `"range"`), `csv`, `tsv`, or `cidr` (one network per line). Default: from the output's extension (`.json`, `.jsonl`/`.ndjson`, `.csv`, `.tsv`), else `json` |
| `--fields` | dotted paths to keep |
| `--flatten`, `--no-header` | CSV and TSV, as in `read` |
| `--where` | `field<op>value`, repeatable, all must match |
| `--family` | `4` or `6` |
| `--range` | comma-separated CIDRs to export from |
| `--aggregate` | with `cidr`, merge adjacent networks into the fewest CIDRs (on by default) |

CSV and TSV fields are quoted only when they hold the delimiter, a quote or
a line break, so `mmdbio import` reads every export format back: JSON Lines
and CSV as they are, and `--flatten`ed CSV with `--nest-keys` (and `--type`
for the numbers).

`--where` takes these operators:

| operator | meaning |
| --- | --- |
| `=` | equals any of the comma-separated values |
| `!=` | equals none of them |
| `>`, `>=`, `<`, `<=` | numeric comparison; numeric strings count as numbers |

- Text comparisons ignore case.
- On an array field, a match on any element counts.
- A record without the field never matches.

### diff

Without flags, `diff` lists the networks added, removed and modified, each
network matched by its exact prefix. Two lookup-based views find what that
misses when boundaries move:

- `--subnets` (`-s`): every network of each file is looked up in the other;
  where it lands in a network of another size, the boundaries changed
  (`old -> new`, with `(no data)` where one side has none). Each change is
  listed once.
- `--records` (`-r`): networks with the same boundaries in both files whose
  records differ, with the old (`-`) and the new (`+`) record.

Both work with `--summary` (counts) and `--json`.

## Shrinking: `mmdbio shrink`

```bash
mmdbio shrink city.mmdb                    # writes city.shrunk.mmdb
mmdbio shrink city.mmdb small.mmdb --record-size 28
mmdbio shrink --dry-run city.mmdb          # the numbers, nothing written
```

`shrink` stores every distinct subtree of the search tree once, drops empty
subtrees, and keeps only the data the tree still reaches (re-encoding its
pointers at the smallest width). The result is a standard MMDB file: every
reader opens it as it is, and every lookup returns what it did.

The method, the checks and the exit codes follow IPinfo's
[`mmdbshrink`](https://github.com/ipinfo/mmdbctl/tree/master/mmdbshrink)
(also `mmdbctl shrink`); this is a separate implementation in C.

Every shrink is checked before it takes the output's name: the metadata
(all but `node_count` and `record_size`), the IPv4 class and reserved-range
boundaries and their IPv6 counterparts, and 1,000,000 random addresses per
family must answer the same in both files. The addresses come from `--seed`
(default 1), so a seed always checks the same ones.

| flag | meaning |
| --- | --- |
| `--out` | the output (or the second argument; default `<input>.shrunk.mmdb`) |
| `--overwrite` | replace an existing output |
| `--dry-run` | report the savings, writing nothing |
| `--record-size` | 24, 28 or 32 for the output (default: the input's); too small fails, naming the smallest that fits |
| `--no-compact` | copy the data section as it is |
| `--check=false` | skip the checks |
| `--full-check` | also compare every network of both files (exhaustive) |
| `--lookups`, `--seed` | the random addresses per family, and their seed |
| `--bench=false` | skip timing the lookups on both files |
| `--bench-rounds` | time in that many interleaved rounds (input then output, then the other way round) and report the medians, the spread and the output/input ratio; 3 or more ride out a noisy round |
| `--full-report` | also measure a reader's memory on each file, in a fresh process (Linux, macOS) |
| `--verbose` (`-v`) | each phase and its time |

| exit code | meaning |
| --- | --- |
| 0 | shrunk and checked |
| 2 | a lookup answered differently (nothing is written) |
| 4 | the metadata differs (nothing is written) |
| 1 | any other error |

The input is mapped, not read into memory. Memory is two bits per input node
plus roughly 30 bytes per distinct node. How much a file shrinks depends on how
much of its tree repeats: dense data with few distinct records shrinks most,
and a shrunk file does not shrink again. `import --shrink` shrinks (and
checks) what it builds.

## Building MMDB files: `mmdbio import`

```bash
mmdbio import data.csv.gz                          # everything detected
mmdbio import ranges.csv --type threat_score=uint16 --type is_vpn=boolean
mmdbio import city_data.json ip-to-city.mmdb --ip 4
mmdbio import --config build.json                  # settings from a file
mmdbio import --config build.json --size 32        # ... with a flag on top
cat ips.txt | mmdbio import - --columns ip --no-fields --out - > list.mmdb
```

The input and the output are `--in` and `--out` or the two arguments.

### What you must provide, and what is defaulted

**You must provide only the input file.** Everything else is detected or has
a default:

| setting | default |
| --- | --- |
| input format | from the extension (`.csv`, `.tsv`, `.txt`, `.json`, `.jsonl`, `.ndjson`, each optionally `.gz`), else from the first byte (`{` or `[` is JSON). Gzip is detected by its magic bytes, whatever the name. |
| CSV delimiter | detected from the first line: comma, tab, semicolon or pipe |
| CSV header | detected. A first row with no IP address in it (and no integer IP up front) is the header. Without one, columns are named `column1`, `column2`, ... |
| IP columns | a column named like `network`, `cidr`, `prefix`, `subnet`, `ip`, `range`, or a pair like `start_ip`/`end_ip`, `ip_from`/`ip_to`, `first_ip`/`last_ip` (any case, `_`/spaces ignored). Failing that, the first column, or the first two if they hold a range. |
| fields | every other column (or JSON key), keyed by its name |
| field types | CSV: every value is a string. JSON: its own types (see below). |
| output | the input path with `.mmdb` in place of its extensions |
| `database_type` | the input file's name (`ip-ranges` for `ip-ranges.csv.gz`) |
| description | `{"en": "Custom IP Intelligence Database"}` |
| languages | `["en"]` |
| IP version | 6: IPv4 lives at `::a.b.c.d`, so one file answers both |
| record size | the smallest of 24, 28 and 32 that fits |
| IPv4 aliasing | off (`--alias-6to4` turns it on) |
| reserved networks | kept (`--disallow-reserved` drops them) |
| overlapping rows | the later row wins (`--merge`) |
| bad rows | skipped, with a warning for each of the first 20 and a count (`--strict` stops at the first) |
| empty values | stored as `""` (`--omit-empty` leaves the key out) |
| addresses no row covers | no data (`--fill-gaps` gives them every field as `""`) |
| the network in the record | not stored (`--network-field` adds `"network"`) |

The only things the tool cannot guess are these:

- the IP columns, when their names say nothing;
- types other than string for CSV values;
- a nested record shape.

Give them as flags or in a config.

### Accepted inputs

| input | shape |
| --- | --- |
| CSV / TSV | a network column, or start and end columns. A network is a CIDR (`1.2.3.0/24`, host bits are cleared), an IP (`8.8.8.8`, `2001:db8::1`), a range (`1.2.3.0-1.2.3.255`) or a decimal integer (`16909056`; up to 4294967295 is IPv4). Quoted fields may hold the delimiter, quotes (`""`) and newlines, and a UTF-8 byte order mark is ignored. |
| JSON object | `{"1.2.3.0/24": {...record...}, ...}`: the format `mmdbio export` writes |
| JSON array | `[{"network": "1.2.3.0/24", ...fields}, ...]` |
| NDJSON | one `{"network": ..., ...}` object per line |

JSON is read one record at a time, so a file of any size works.

IPv4-mapped addresses (`::ffff:1.2.3.4`) count as IPv4. Rows may come in any
order and overlap. They are sorted on disk, past the `--memory` buffer
(512 MB by default), so input size is not limited by RAM.

mmdbctl's exports import as they are: CSV with a `range` column, JSON Lines
with a `range` key, and `start_ip,end_ip,join_key` CSV (add
`--exclude join_key`).

More shaping options:

| flag | effect |
| --- | --- |
| `--columns a,b,c` | names for the CSV's columns, in order (a headerless file's are otherwise `column1`, `column2`, ...) |
| `--network-field` | each record gets `"network"`: the row's CIDR, or its range as `start-end` |
| `--no-fields` | records hold no columns: an IP list becomes a membership database (only `"network"` with `--network-field`) |
| `--fill-gaps` | addresses no row covers get a record with every field `""`; rows always win, under every `--merge` mode, and with `--merge toplevel` every row gets the fields it lacks as `""` too |
| `--out -` | the MMDB to stdout (never to a terminal) |
| `--shrink` | shrink and check the result, as `mmdbio shrink` does |
| `--rename column=key` | store a column (or top-level JSON key) under another name; repeatable |
| `--empty-if column=value` | read that value of the column as empty (`bot_type=0`); repeatable, and a column may have several |
| `--lookup FILE` | take each record from a row of another CSV (see below) |

`--include`, `--exclude`, `--type` and `--empty-if` name the input's columns;
`--rename` decides the key the record gets.

**Lookups.** With `--lookup ids.csv`, each input row gives only its network
and an id; the record is the lookup row with that id. `--lookup-key` names
the lookup's id column (default: its first), and `--lookup-on` the input's
(default: the same name). The lookup's other columns are the fields, with
`--include`, `--exclude`, `--type`, `--rename` and `--empty-if` as usual.
An id that repeats keeps its last row; an id the lookup lacks gets a record
with every field `""` (counted in the log). The lookup is indexed on disk:
only where each id's row starts is kept in memory, and a gzipped lookup is
unpacked into the temporary directory first.

```bash
mmdbio import ranges.csv asn.mmdb --lookup asn-details.csv --lookup-key id --lookup-on asn_id
```

**mmdbctl's flags** work too, so its import commands run as they are:
`--csv`, `--tsv`, `--json`/`-j`, `--fields`/`-f` (names for a headerless
CSV's columns after the range ones), `--fields-from-header`,
`--range-multicol`, `--joinkey-col`, `--no-network` (the default here),
`--ignore-empty-values` (= `--fill-gaps`), `--disable-metadata-pointers`
(accepted: mmdbio never writes pointers in the metadata), `-s` and `-m`.
mmdbctl's `-c` and `-t` stay mmdbio's `--config` and `--title`.

### Supported types

| type | MMDB type | from a CSV value | from JSON |
| --- | --- | --- | --- |
| `string` | utf8_string | as is | strings as is; numbers and booleans as their text |
| `uint16` | uint16 | digits only, 0 to 65,535 | an integer in range |
| `uint32` | uint32 | 0 to 4,294,967,295 | an integer in range |
| `uint64` | uint64 | 0 to 2^64 - 1 | an integer in range |
| `uint128` | uint128 | 0 to 2^128 - 1 | an integer in range |
| `int32` | int32 | -2^31 to 2^31 - 1 | an integer in range |
| `double` | double | a decimal or exponent number | any number |
| `float` | float | a decimal or exponent number | any number |
| `boolean` | boolean | `true`/`false`, `yes`/`no`, `t`/`f`, `y`/`n`, `1`/`0` (any case) | `true`/`false` |
| `array` | array of utf8_string | split on `--array-separator` (`,`), outer `[ ]` and item quotes removed | a JSON array |
| `auto` | inferred | integers by size, other numbers as double, `true`/`false` as boolean, anything else (`007` included) a string | as JSON |

Rules that apply to every type:

- **Empty values.** An empty value of a number or boolean type leaves the key
  out. A value its type rejects makes the row bad (skipped with a warning).
- **JSON's own types** are what a field gets without a type rule:
  - a whole number becomes uint32, uint64 or uint128 (or int32 when
    negative) by size;
  - any other number becomes a double;
  - `true`/`false` become booleans;
  - `null` becomes `""`;
  - objects and arrays stay nested.
- **Type rules on JSON** use dotted paths (`location.latitude`), with `[]`
  for array elements (`tags[]`).
- **Invalid UTF-8** in a value becomes U+FFFD, so the file always passes
  verification.

Not supported yet: bytes fields, typed arrays from CSV (arrays from CSV are
arrays of strings), and nested maps from CSV beyond `--nest-keys`'s dotted
names.

### Overlapping rows (`--merge`)

| value | where rows overlap |
| --- | --- |
| `none` (or `replace`, the default) | the later row wins |
| `keep` | the earlier row wins |
| `toplevel` | the records' top-level keys are merged, later keys winning |
| `recurse` | the records are merged recursively (maps by key, arrays by index) |

"Later" means later in the file.

### The config file

A JSON file with any of these keys; see `examples/import-config.json`.

- Keys starting with `_` are ignored, so they can hold comments.
- Unknown keys are errors, which catches typos.
- Relative paths are taken from the config file's directory.
- Precedence: built-in defaults, then the config, then flags given on the
  command line.

| key | flag | meaning |
| --- | --- | --- |
| `input` (string) or `input.path` | `--in` | the input file (`-` is stdin) |
| `input.format` | `--format` | `auto`, `csv`, `tsv`, `json`, `jsonl` |
| `input.delimiter` | `--delimiter` | one character, or `comma`, `tab`, `semicolon`, `pipe`, `space` |
| `input.header` | `--header` | `true`, `false` or `"auto"` |
| `input.lazy_quotes` | `--lazy-quotes` | accept stray quotes in CSV fields |
| `input.columns` | `--columns` | `["network", "country", ...]` |
| `lookup` (string) or `lookup.path` | `--lookup` | the lookup CSV |
| `lookup.key_column`, `lookup.input_column` | `--lookup-key`, `--lookup-on` | its id column, and the input's |
| `output` | `--out` | the MMDB file |
| `network` (string) or `network.column` | `--network-column` | the network column |
| `network.start_column`, `network.end_column` | `--start-column`, `--end-column` | a range's columns |
| `metadata.database_type` | `--title` | |
| `metadata.description` | `--description` | a string (English), or `{"en": "...", "de": "..."}` |
| `metadata.languages` | `--languages` | `[]` writes none |
| `metadata.ip_version` | `--ip` | 4 or 6 |
| `metadata.record_size` | `--size` | 24, 28, 32 or `"auto"` |
| `tree.alias_ipv4` | `--alias-6to4` | map IPv4 into `::ffff:0:0/96`, `2001::/32` and `2002::/16` |
| `tree.include_reserved` | `--disallow-reserved` (inverted) | keep data in reserved networks |
| `tree.fill_gaps` | `--fill-gaps` | `true` or `false` |
| `merge` | `--merge` | see above |
| `on_error` | `--strict` | `skip` or `fail` |
| `fields.include`, `fields.exclude` | `--include`, `--exclude` | columns (or top-level JSON keys) to keep or drop |
| `fields.types` | `--type field=type` | `{"threat_score": "uint16", ...}` |
| `fields.infer_types` | `--infer-types` | type CSV values by their look (`auto`) |
| `fields.nest_keys` | `--nest-keys` | `location.city` columns become nested maps |
| `fields.empty` | `--omit-empty` | `keep` or `omit` |
| `fields.array_separator` | `--array-separator` | |
| `fields.network_field` | `--network-field` | `true` or `false` |
| `fields.no_fields` | `--no-fields` | `true` or `false` |
| `fields.rename` | `--rename` | `{"bot_owner_name": "bot_operator_name"}` |
| `fields.empty_if` | `--empty-if` | `{"bot_type": "0", "country": ["0", "-"]}` |
| `shrink` | `--shrink` | `true` or `false` |
| `temp_dir` | `--tmp-dir` | where the sort and record files go (default `$TMPDIR`, else the system's temporary directory) |
| `memory_mb` | `--memory` | the in-memory sort buffer |

## The library

`make lib` builds `build/libmmdbio.a`; its API is `include/mmdbio.h`, and
only its `mmdbio_` names are exported. It does what the command does:

| function | what it does |
| --- | --- |
| `mmdbio_open`, `mmdbio_close`, `mmdbio_get_metadata` | map a database, read its metadata |
| `mmdbio_lookup` | an address's record as JSON, and the network it matched |
| `mmdbio_networks` | every network with data, in order, to a callback |
| `mmdbio_verify` | the checks of `mmdbio verify` |
| `mmdbio_shrink`, `mmdbio_check` | shrink a file; check two files answer alike |
| `mmdbio_bench`, `mmdbio_bench_paired` | time lookups, one file or two in interleaved rounds |
| `mmdbio_build` | build from an import config file |

```c
#include "mmdbio.h"

mmdbio_db *db;
if (mmdbio_open("city.mmdb", &db) == 0) {
    int found;
    char *json, *network;
    if (mmdbio_lookup(db, "8.8.8.8", &found, &json, &network) == 0 && found)
        printf("%s %s\n", network, json);   /* free() both */
    mmdbio_close(db);
}
```

```bash
cc -Iinclude my.c build/libmmdbio.a -lm      # -lpsapi too on Windows
```

`examples/lib_example.c` is a small lookup tool, and `tests/lib_test.c`
exercises every function.

## Checked against the production writers

The three production databases, built with `mmdbio import` from the
production inputs and compared with the files the Perl scripts and the C
`mmdb-writer` build from the same inputs. Each import config is the
generic writer's entry put as mmdbio settings: lookups for the ASN and abuse
records, `--rename` and `--empty-if` for the security fields.

| database | mmdbio build | the same as Perl and C |
| --- | --- | --- |
| asn-details (39.4M rows, 136,594 lookup ids) | 1:31, 1.6 GB | every lookup, with types; the same 1,299,297 nodes; the tree and data byte for byte as C's |
| abuse-details (39.4M rows, 15.3M lookup ids) | 2:25, 3.2 GB | every lookup, with types; the same 19,741,113 nodes; the tree and data (965 MB) byte for byte as C's |
| ip-proxy-v5 (71M rows, 81.6M networks) | 9:39, 1.6 GB | every lookup, with types; the same 1,394,771,861 nodes; the tree and data (13.4 GB) byte for byte as C's |

For the security database the Perl script needs about 140 GB of memory; the
C writer 8:56 and 1.9 GB. Building again gives the same tree and data, byte
for byte; only the build time in the metadata changes.

Shrinking them keeps every lookup: asn-details loses 24% of its nodes (13%
of the file), abuse-details 3.9% of its nodes (0.6%), in 11 seconds and
1.8 GB for the abuse file, and ip-proxy-v5 21% of its nodes (13.4 GB to
11.1 GB) in 5:29 and 41 GB, the mapped 13.4 GB input included.

## Differences from the Go mmdbio

The commands, flags and output formats are the same; everything new is
added beside them (arguments for files, new formats and flags, `shrink`).
These things changed:

- **`import` reads CSV, TSV, JSON arrays and NDJSON**, each gzipped or not, as
  well as the old JSON object. It takes a config file, and the old flags
  still work.
- **`import` follows the file order.** The Go version took entries in random
  order, so overlapping rows could end differently on every run. Here the
  later row wins, which is what Go's `--merge none` did when the order
  happened to match the file; `keep` is new.
- **JSON integers stay integers.** The Go version stored every number as a
  double; integers here become uint32 / uint64 / uint128 / int32 by size, and
  `--type` picks any MMDB type.
- **`--size` defaults to `auto`** (Go: 32), and `--title` defaults to the
  input's name (Go: `Custom-ip-database`).
- **`export` writes networks in address order** rather than sorted as
  strings. Same keys, same values. Each record is decoded once and the JSON
  is streamed, so memory stays flat on large files.
- **`diff` and `stats` count each network once.** The Go version also walked
  the aliased IPv4 copies inside IPv6 files and printed IPv4 networks of an
  IPv6 file as `::102:300/120`. Here IPv4 networks are listed as IPv4 (as
  `export` already did), and the lists are in address order.
- **`inspect` lists fields sorted by path** (Go: random order).
- **`verify` exists.** It was documented but missing from the Go source.
- **Shell completion** asks the program, as Go's did (`mmdbio __complete`),
  and also offers flag values and `.mmdb` files; `completion install` sets it up.
- **`export` without `--out`** writes to stdout (Go: an error).

## Development

```
src/            the tool
  main.c, flags.*        command line (pflag rules), help, dispatch
  cmd_*.c                the commands
  mmdb_read.*            reader: mapping, decoder, network walk, lookups, verify
  shrink.*, shrink_check.c   shrink: interning the tree, compacting the data; the checks
  tabular.*              CSV, TSV and JSON Lines rows for read and export
  libmmdbio.c            the library's API (include/mmdbio.h)
  compat.*, color.*      what differs between systems (Windows above all); colours
  jsonw.*                JSON output as Go's json.MarshalIndent writes it
  netaddr.*              IP and network text: parsing, formatting, ranges to CIDRs
  input.*                files, stdin and gzip (bundled zlib)
  csv.*, jstream.*       CSV (Go's encoding/csv rules) and streaming JSON
  rows.*                 rows of an input: detection, fields, types
  bconfig.c, build.h     import settings and the config file
  engine.c               the build: record store, sort runs, overlap sweep
  streamwriter.*         the streaming tree writer (two passes, tree on disk)
  mmdb_enc.*             data-section encoding and deduplication
tests/
  unit_tests.c           addresses, Go number formatting, flags, types, JSON and CSV output
  run_tests.sh           end-to-end: the commands, the CLI, and the checkers below
  check_build.py         import against a brute-force model of every address
  check_robust.py        corrupted and malformed inputs fail cleanly
  check_shrink.py        shrink on hand-built files (every pointer width, unreached data)
  check_features.py      arguments, the new formats, diff by lookup, import options, completion
  check_edges.py         databases built with every feature and input shape, checked with
                         the independent reader, then read, exported, re-imported and shrunk
  lib_test.c             the library, through its header only
  mmdb_py.py             an independent MMDB writer and reader for those checks
packaging/               .deb and .rpm builders, the Homebrew formula
install.sh, install.ps1  the release installers
third_party/zlib/        zlib 1.3.1 (inflate only), zlib license
```

## License

Apache License 2.0; see `LICENSE`. The bundled zlib is under the zlib license
(`third_party/zlib/LICENSE`).
