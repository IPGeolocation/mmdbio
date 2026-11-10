# mmdbio

mmdbio is a command-line tool for MMDB files. Use it to look up IP
addresses, export data, compare two files, check a file, make a file smaller,
and build your own MMDB file from CSV or JSON.

It is one program with nothing else to install, for Linux, macOS, Windows and
the BSDs. It replaces the older Go version of mmdbio, and all the old
commands and options still work.

## Install

| System | Command |
| --- | --- |
| Linux or macOS | `curl -fsSL https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.sh \| sh` |
| Windows (PowerShell) | `irm https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.ps1 \| iex` |
| Debian or Ubuntu | `sudo apt install ./mmdbio_<version>_amd64.deb` (from the [releases page](https://github.com/IPGeolocation/mmdbio/releases)) |
| Fedora, RHEL or SUSE | `sudo rpm -i mmdbio-<version>-1.x86_64.rpm` (from the [releases page](https://github.com/IPGeolocation/mmdbio/releases)) |

```bash
mmdbio --version
```

To build from source (use `gmake` on the BSDs and illumos):

```bash
git clone https://github.com/IPGeolocation/mmdbio.git
cd mmdbio
make
sudo make install
```

## Commands

| Command | What it does |
| --- | --- |
| [`import`](#import) | Build an MMDB file from a CSV or JSON file. |
| [`read`](#read) | Look up IP addresses. |
| [`export`](#export) | Write out the data, or a list of IP blocks. |
| [`shrink`](#shrink) | Make a file smaller without changing any answer. |
| [`metadata`, `stats`, `inspect`, `verify`](#metadata-stats-inspect-verify) | Show information about a file, or check it. |
| [`diff`](#diff) | Compare two files. |
| [`completion`](#completion) | Set up Tab completion in your shell. |

`mmdbio <command> --help` lists every option of a command. Files and
addresses can also be given without their flags, for example
`mmdbio read 8.8.8.8 ips.mmdb`.

### import

Build an MMDB file from a CSV, TSV, JSON or NDJSON file, gzipped or not.
The examples in this README use this `ips.csv`:

```csv
network,country,city,asn,is_vpn,score
8.8.8.0/24,US,Mountain View,15169,false,5
1.1.1.0/24,AU,Sydney,13335,false,3
2.56.0.0/16,DE,Berlin,64496,true,80
2001:db8::/32,NL,Amsterdam,64500,true,90
```

```bash
mmdbio import --in ips.csv
```

```text
input: CSV (comma-delimited, with a header); network column "network"; 5 fields
rows: 4 read, 4 inserted, 0 skipped
tree: 4 unique records, 177 search-tree nodes, record size 24, 1,455 bytes
✅ Successfully wrote 4 entries to ips.mmdb
```

The `input:` line shows the column mmdbio used for the IPs. Every other
column becomes a text field.

```bash
mmdbio import --in ips.csv --out city.mmdb       # pick the output name (default: ips.mmdb)
mmdbio import --in ips.csv.gz                    # gzipped input
mmdbio import --in data.json                     # JSON or NDJSON
mmdbio import --in ips.csv --title City --description "My city data"
mmdbio import --config build.json                # settings from a config file
```

To keep the settings in a file, see [Config file](#config-file).

#### The IP column

mmdbio finds the IP column by these names (case, `_`, `-` and spaces don't
matter, so `Start IP` works):

| Your file has | Names |
| --- | --- |
| one IP column | `network`, `cidr`, `prefix`, `subnet`, `ip_range`, `range`, `ip_network`, `net`, `block`, `netblock`, `ip`, `ip_address`, `address`, `ips`, `ip_range_cidr` |
| a first address | `start_ip`, `ip_start`, `start`, `from`, `ip_from`, `from_ip`, `first_ip`, `ip_first`, `range_start`, `start_address`, `begin_ip`, `network_start_ip`, `start_ip_address`, `low_ip`, `ip_low`, `min_ip` |
| a last address | `end_ip`, `ip_end`, `end`, `to`, `ip_to`, `to_ip`, `last_ip`, `ip_last`, `range_end`, `end_address`, `network_last_ip`, `end_ip_address`, `high_ip`, `ip_high`, `max_ip`, `stop_ip` |

The values can be `8.8.8.0/24`, `8.8.8.8`, `8.8.8.0-8.8.8.255`, a number
like `134744064`, or IPv6.

| Your file starts with | Command |
| --- | --- |
| `network,country` (or `cidr`, `ip`, `start_ip,end_ip`, ...) | `mmdbio import --in file.csv` |
| `addr,country` | `mmdbio import --in file.csv --network-column addr` |
| `first,last,country` | `mmdbio import --in file.csv --start-column first --end-column last` |
| `cidr,start_ip,end_ip,country` | `mmdbio import --in file.csv --exclude cidr` (it uses `start_ip` and `end_ip`) |
| `8.8.8.0/24,US` (no header) | `mmdbio import --in file.csv --columns network,country` |
| `8.8.8.0,8.8.8.255,US` (no header) | `mmdbio import --in file.csv --columns start_ip,end_ip,country` |
| `US,8.8.8.0/24` (no header) | `mmdbio import --in file.csv --columns country,network` |

Without `--columns`, a file with no header gets fields named `column2`,
`column3`, and so on. Comma, tab, semicolon and pipe are found on their own;
for anything else use `--delimiter`.

#### Types

```bash
mmdbio import --in ips.csv --out typed.mmdb --type asn=uint32 --type is_vpn=boolean --type score=uint16
mmdbio read --db typed.mmdb --ip 8.8.8.8 -f jsonl
```

```text
{"asn":15169,"city":"Mountain View","country":"US","ip":"8.8.8.8","is_vpn":false,"score":5}
```

Types: `string` (default), `uint16`, `uint32`, `uint64`, `uint128`, `int32`,
`double`, `float`, `boolean`, `array` and `auto`. `--infer-types` guesses
every column. `--type tags=array` reads `[dns,google]` as a list
(`--array-separator ';'` changes the separator). A value that doesn't fit its
type skips the row with a warning.

#### Fields

```bash
mmdbio import --in ips.csv --include country,city             # only these columns
mmdbio import --in ips.csv --exclude asn,score                # leave these out
mmdbio import --in ips.csv --rename country=country_code      # store a column under another name
mmdbio import --in ips.csv --nest-keys                        # location.city becomes {"location": {"city": ...}}
mmdbio import --in ips.csv --omit-empty                       # leave out empty values
mmdbio import --in ips.csv --empty-if country=-               # treat "-" as empty
mmdbio import --in ips.csv --network-field                    # add "network" to each record
mmdbio import --in list.txt --columns ip --no-fields          # a plain IP list, no fields
```

#### Overlaps and bad rows

```bash
mmdbio import --in file.csv                    # rows that overlap: the later row wins
mmdbio import --in file.csv --merge keep       # the earlier row wins
mmdbio import --in file.csv --merge toplevel   # combine both records (recurse: nested parts too)
mmdbio import --in file.csv --strict           # stop at the first bad row (default: skip it with a warning)
```

#### JSON

| File | Command |
| --- | --- |
| `{"8.8.8.0/24": {"country": "US", "asn": 15169}}` | `mmdbio import --in data.json` |
| `[{"network": "8.8.8.0/24", "country": "US"}]` | `mmdbio import --in data.json` |
| `{"start_ip": "8.8.8.0", "end_ip": "8.8.8.255", "country": "US"}`, one per line | `mmdbio import --in data.jsonl` |

JSON values keep their types. `--type location.latitude=double` sets the type
of a nested field.

#### Details from a second file

```text
ranges.csv:       start_ip,end_ip,asn_id
asn-details.csv:  id,asn,name,country
```

```bash
mmdbio import --in ranges.csv --out asn.mmdb --lookup asn-details.csv --lookup-key id --lookup-on asn_id
```

#### Settings of the file you build

| Option | Sets | Default |
| --- | --- | --- |
| `--out file` | output file (`-` for standard output) | the input name with `.mmdb` |
| `--title` | database type | the input file's name |
| `--description` | description | `Custom IP Intelligence Database` |
| `--languages en,de` | languages | `en` |
| `--ip 4` or `6` | IPv4 only, or IPv4 and IPv6 | `6` |
| `--size 24`, `28`, `32` | size of each link | the smallest that fits |
| `--alias-6to4` | IPv6 forms of an IPv4 address give the IPv4 answer | off |
| `--disallow-reserved` | drop private and special-use blocks | off |
| `--shrink` | shrink and check the file after building it | off |
| `--memory MB` | memory for sorting rows | 512 |

### read

```bash
mmdbio read --db ips.mmdb --ip 8.8.8.8
```

```text
{
  "asn": "15169",
  "city": "Mountain View",
  "country": "US",
  "is_vpn": "false",
  "score": "5"
}
```

`ips.txt` has `8.8.8.8`, `1.1.1.1` and `9.9.9.9`, one per line:

```bash
mmdbio read --db ips.mmdb --input ips.txt
```

```text
{
  "1.1.1.1": { "asn": "13335", "city": "Sydney", ... },
  "8.8.8.8": { "asn": "15169", "city": "Mountain View", ... },
  "9.9.9.9": null
}
```

```bash
mmdbio read --db ips.mmdb --range 2.56.0.0/30 -f csv
```

```text
ip,asn,city,country,is_vpn,score
2.56.0.0,64496,Berlin,DE,true,80
2.56.0.1,64496,Berlin,DE,true,80
2.56.0.2,64496,Berlin,DE,true,80
2.56.0.3,64496,Berlin,DE,true,80
```

```bash
mmdbio read --db ips.mmdb --range 1.1.1.0/29                 # every address of a CIDR
cat ips.txt | mmdbio read --db ips.mmdb --input -            # addresses from standard input
mmdbio read --db ips.mmdb --ip 8.8.8.8 --fields city,country
mmdbio read --db ips.mmdb --input ips.txt -f jsonl --out result.jsonl
```

| Option | What it does |
| --- | --- |
| `--db file` | The MMDB file. |
| `--ip` | One address. |
| `--range` | Every address of a CIDR. |
| `--input file` | A file with one address per line (`-` for standard input). |
| `-f` | `json` (default), `jsonl`, `json-pretty`, `csv` or `tsv` |
| `--fields a,b` | Only these fields. Nested fields: `location.city.name`. |
| `--flatten` | CSV/TSV: one column per nested field. |
| `--no-header` | CSV/TSV without the header line. |
| `--out file` | Write to a file (one address in JSON always prints on the screen). |

### export

```bash
mmdbio export --db ips.mmdb --out out.csv
```

```text
Exported 4 records to out.csv
```

```csv
range,asn,city,country,is_vpn,score
1.1.1.0/24,13335,Sydney,AU,false,3
2.56.0.0/16,64496,Berlin,DE,true,80
8.8.8.0/24,15169,Mountain View,US,false,5
2001:db8::/32,64500,Amsterdam,NL,true,90
```

The file ending picks the format: `.json`, `.jsonl`, `.csv` or `.tsv`.
Without `--out`, JSON goes to the screen.

A list of IP blocks, for example for a firewall:

```bash
mmdbio export --db ips.mmdb --where is_vpn=true -f cidr
```

```text
2.56.0.0/16
2001:db8::/32
Exported 2 networks as 2 CIDRs to stdout
```

```bash
mmdbio export --db ips.mmdb -f jsonl --where 'score>=50'          # quote conditions that have > or <
mmdbio export --db ips.mmdb --where country=DE,FR                  # DE or FR
mmdbio export --db ips.mmdb --where country!=US                    # not US
mmdbio export --db ips.mmdb --where is_vpn=true --where 'score>50' # both must match
mmdbio export --db ips.mmdb --fields city,score                    # only these fields
mmdbio export --db ips.mmdb --family 4                             # only IPv4 networks
mmdbio export --db ips.mmdb --range 8.8.0.0/16                     # only networks inside this block
```

| Option | What it does |
| --- | --- |
| `--db file` | The MMDB file. |
| `--out file` | The output file (`-` or none: the screen). |
| `-f` | `json`, `jsonl`, `csv`, `tsv`, or `cidr` (only the blocks, with neighbours joined; `--aggregate=false` lists them as stored) |
| `--where` | `field=a,b`, `field!=a,b`, `field>N` (also `>=`, `<`, `<=`). Text ignores case. |
| `--fields a,b` | Only these fields. |
| `--family 4` or `6` | Only IPv4 or only IPv6 networks. |
| `--range a,b` | Only networks inside these blocks. |
| `--flatten`, `--no-header` | As in `read`. |

### shrink

```bash
mmdbio shrink --db city.mmdb                       # writes city.shrunk.mmdb
mmdbio shrink --db city.mmdb --out small.mmdb      # pick the output name
mmdbio shrink --db city.mmdb --dry-run             # only show the savings
mmdbio shrink --db city.mmdb --record-size 24      # also use smaller links (24, 28 or 32)
```

`ips32.mmdb` is `ips.csv` built with `--size 32`:

```bash
mmdbio shrink --db ips32.mmdb --record-size 24 --dry-run
```

```text
input:   ips32.mmdb
output:  (dry run, nothing written)

  size:   1.81 KB      ->  1.46 KB      (saved 354 B, -19.57%)
  nodes:  177          ->  177          (+0.00%)
  tree:   1.42 KB      ->  1.06 KB
  data:   163 B        ->  163 B        (unchanged)
  record size: 32 -> 24 bits
```

Before writing, `shrink` checks that both files give the same answer for 41
edge addresses and for 1,000,000 random IPv4 and IPv6 addresses. If any answer
differs, it writes nothing and exits with status 2 (4 if the metadata
differs).

| Option | What it does |
| --- | --- |
| `--overwrite` | Replace the output file if it exists. |
| `--full-check` | Also compare every network of both files. |
| `--lookups N` | Random addresses to check (default 1,000,000). |
| `--check=false` | Skip the checks. |
| `--bench=false` | Skip timing lookups on both files. |
| `-v` | Show each step and its time. |

### metadata, stats, inspect, verify

```bash
mmdbio metadata --db ips.mmdb
```

```text
MMDB Metadata
-----------------------------
Database Type: ips
IP Version:    6
Record Size:   24 bits
Node Count:    177
Build Date:    Tue, 10 Nov 2026 21:05:55 PKT
Languages:     [en]
Description: {
    "en": "Custom IP Intelligence Database"
  }
```

```bash
mmdbio metadata --db ips.mmdb --json               # as JSON
mmdbio metadata --db ips.mmdb --layout             # also the size of each part of the file
mmdbio stats --db ips.mmdb                         # the metadata, plus "IPv4 Count: 3", "IPv6 Count: 1", "Networks: 4"
mmdbio inspect --db ips.mmdb --sample-ip 8.8.8.8   # the fields of one record and their types ("asn : string", ...)
mmdbio verify --db ips.mmdb                        # "valid", or "invalid: <reason>" with exit status 1
```

### diff

`ips-new.mmdb` is a newer build: Sydney is now Melbourne, `2.56.0.0/16` is
cut to `2.56.0.0/17`, `9.9.9.0/24` is added and `2001:db8::/32` is removed.

```bash
mmdbio diff --old ips.mmdb --new ips-new.mmdb
```

```text
Added: 2
Removed: 2
Modified: 1

Added:
   2.56.0.0/17
   9.9.9.0/24
Removed:
   2.56.0.0/16
   2001:db8::/32
Modified:
   1.1.1.0/24
```

```bash
mmdbio diff --old ips.mmdb --new ips-new.mmdb -s -r
```

```text
Subnets changed: 3
   2.56.0.0/16 -> 2.56.0.0/17
   9.0.0.0/8 (no data) -> 9.9.9.0/24
   2001:db8::/32 -> 2000::/3 (no data)

Records changed: 1
   1.1.1.0/24
      - {"asn":"13335","city":"Sydney","country":"AU","is_vpn":"false","score":"3"}
      + {"asn":"13335","city":"Melbourne","country":"AU","is_vpn":"false","score":"3"}
```

```bash
mmdbio diff --old ips.mmdb --new ips-new.mmdb --summary   # Added: 2 | Removed: 2 | Modified: 1
mmdbio diff --old ips.mmdb --new ips-new.mmdb --json      # with the full records
```

### completion

```bash
mmdbio completion install     # bash, zsh, fish or PowerShell; open a new terminal after
mmdbio completion uninstall
```

## Config file

```json
{
  "input": "ips.csv",
  "output": "typed.mmdb",
  "metadata": {"database_type": "Demo-City", "description": {"en": "Demo city database"}},
  "fields": {"types": {"asn": "uint32", "is_vpn": "boolean", "score": "uint16"}}
}
```

```bash
mmdbio import --config build.json
mmdbio import --config build.json --size 32   # an option on the command line wins
```

`${name}` in a path comes from `vars`, and `--var` changes it for one run:

```json
{"vars": {"root": "~/data"}, "input": "${root}/ranges.csv", "output": "${root}/ranges.mmdb"}
```

```bash
mmdbio import --config build.json --var root=/data/samples
```

Paths are relative to the config file. Keys starting with `_` are notes.
`examples/import-config.json` shows most keys.

| Key | Same as |
| --- | --- |
| `input`, or `input.path`, `.format`, `.delimiter`, `.header`, `.columns` | `--in`, `--format`, `--delimiter`, `--header`, `--columns` |
| `output` | `--out` |
| `network`, or `network.column`, `.start_column`, `.end_column` | `--network-column`, `--start-column`, `--end-column` |
| `lookup`, or `lookup.path`, `.key_column`, `.input_column` | `--lookup`, `--lookup-key`, `--lookup-on` |
| `metadata.database_type`, `.description`, `.languages`, `.ip_version`, `.record_size` | `--title`, `--description`, `--languages`, `--ip`, `--size` |
| `tree.alias_ipv4`, `.include_reserved`, `.fill_gaps` | `--alias-6to4`, the opposite of `--disallow-reserved`, `--fill-gaps` |
| `fields.include`, `.exclude`, `.types`, `.infer_types`, `.nest_keys`, `.rename`, `.empty_if`, `.array_separator`, `.network_field`, `.no_fields` | the import options of the same name (`types` is `--type`) |
| `fields.empty`: `"omit"` | `--omit-empty` |
| `merge`, `order`, `on_error`: `"fail"` | `--merge`, `--order`, `--strict` |
| `shrink`, `temp_dir`, `memory_mb` | `--shrink`, `--tmp-dir`, `--memory` |
| `record` | a [record template](#record-templates) |

## Record templates

A record template builds nested records from several CSV files. It goes under
`record` in a config file.

```text
ranges.csv:     start_ip,end_ip,country_id,city_name_id,latitude,radius,time_zone
countries.csv:  id,iso_code,name_id
names.csv:      id,name_en,name_de
```

```json
{
  "input": "${root}/ranges.csv",
  "output": "${root}/city.mmdb",
  "vars": {"root": "~/data"},
  "order": "ipv6-first",
  "record": {
    "tables": {
      "countries": "${root}/countries.csv",
      "names": {"path": "${root}/names.csv", "key": "id", "trim": false}
    },
    "trim": true,
    "types": {"latitude": "double", "accuracy_radius": "uint16"},
    "define": {"localized": {"en": "name_en", "de": "name_de"}},
    "skip_if": [{"value": {"$table": "countries", "$on": "country_id", "$col": "iso_code"}, "in": ["", "ZZ"]}],
    "shape": {
      "country": {"$table": "countries", "$on": "country_id",
                  "iso_code": "iso_code",
                  "names": {"$use": "localized", "$table": "names", "$on": "name_id"}},
      "city": {"names": {"$use": "localized", "$table": "names", "$on": "city_name_id"}},
      "location": {"latitude": "latitude", "accuracy_radius": {"$col": "radius", "$ceil": true}},
      "time_zone": "time_zone"
    }
  }
}
```

A row of `ranges.csv` becomes:

```json
{"country": {"iso_code": "DE", "names": {"en": "Germany", "de": "Deutschland"}},
 "city": {"names": {"en": "Munich", "de": "München"}},
 "location": {"latitude": 48.137, "accuracy_radius": 20},
 "time_zone": "Europe/Berlin"}
```

| In `record` | What it does |
| --- | --- |
| `shape` | The record. |
| `tables` | Extra CSV files, by name. `key` is the id column (default: the first). |
| `define` | Named parts, used with `$use`. |
| `types` | The type of every value stored under a name. |
| `trim` | `true`: remove spaces around every value. |
| `empty` | `keep` (default) or `omit` empty values. |
| `invalid` | When a check fails: `bad` (skip the row with a warning), `omit` (drop the value) or `skip` (skip the row). |
| `skip_if` | Skip rows whose value is in the list. |
| `skip_empty` | `true`: skip rows whose record is empty. |

| In `shape` | What it does |
| --- | --- |
| `"column"` | That column (`"column?"` if it may be missing). |
| `{...}`, `[...]` | A map, a list. |
| `12`, `true`, `{"$const": ...}` | A fixed value. |
| `{"$table": "t", "$on": "column", ...}` | Read the inside from table `t`, at the row whose id is in `column`. `"$missing"`: `empty`, `omit` or `skip`. |
| `{"$use": "name"}` | A part from `define`. |
| `{"$first": [...]}` | The first value that isn't empty. |
| `{"$col": "column", ...}` | A column, with any of these steps, run in this order: `$trim`, `$case` (`upper`/`lower`), `$empty_if` (`["0", "-"]`), `$strip_prefix` (`{"equals": "country_code", "sep": "-"}`), `$one_of` (`["a", "b"]`), `$chars` (`"A-Z0-9"`), `$length` (`[2, 3]`), `$number` (`[-90, 90]`), `$ceil`, `$default` (`"unknown"`), `$parse` (`literal` or `list`), `$type` (`"uint16"`). |

## C library

```bash
make lib    # build/libmmdbio.a and include/mmdbio.h
```

```c
#include "mmdbio.h"

mmdbio_db *db;
if (mmdbio_open("city.mmdb", &db) == 0) {
    int found;
    char *json, *network;
    if (mmdbio_lookup(db, "8.8.8.8", &found, &json, &network) == 0 && found)
        printf("%s %s\n", network, json);   /* free() both when done */
    mmdbio_close(db);
}
```

```bash
cc -Iinclude my.c build/libmmdbio.a -lm    # add -lpsapi on Windows
```

`include/mmdbio.h` lists every function, and `examples/lib_example.c` is a
working example.

## Upgrading from the Go version

All commands and options of the Go version (1.x) still work. What changed:

- `import` reads CSV, TSV, JSON lists and NDJSON (gzipped or not), and takes
  a config file.
- `import` follows file order: the later row wins. Whole numbers in JSON stay
  whole numbers.
- `--size` defaults to the smallest that fits (was 32), and `--title` to the
  input file's name.
- `export` lists networks in order and works on very large files; without an
  output file it writes to the screen.
- `diff` and `stats` count each network once and show IPv4 networks as IPv4.
- New: `verify`, `shrink`, `completion install`, CSV/TSV/JSON Lines output,
  and files as plain arguments.

## Development

```bash
make test            # run the tests (needs python3)
./build-release.sh   # build, test and pack every release file into dist/
```

`build-release.sh` also shows how to cross-compile for 32-bit, ARM and
Windows. Pushing a tag like `v2.0.0` publishes a release; a tag with a hyphen,
like `v2.0.0-rc.1`, is a pre-release.

## License

Apache License 2.0; see `LICENSE`. The included zlib is under the zlib
license (`third_party/zlib/LICENSE`).
