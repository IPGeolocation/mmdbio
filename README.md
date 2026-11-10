# mmdbio

mmdbio is a command-line tool for working with MMDB files.

An MMDB file is a small, fast database of IP addresses. It stores blocks of
IP addresses, and for each block it stores some information, such as the
country, the city or the company that uses those addresses. Programs use MMDB
files to look up an IP address on their own computer, without asking an
online service.

With mmdbio you can:

- look up one IP address, many IP addresses, or whole blocks of them;
- export the data in a file to JSON, CSV or TSV, or export a list of IP
  blocks (for example, all VPN blocks) for a firewall;
- compare two versions of a file and see what changed;
- check that a file is valid;
- see what information a file holds;
- make a file smaller without changing any answer it gives;
- build your own MMDB file from a CSV or JSON file.

mmdbio is one small program. It needs nothing else installed, and it works on
Linux, macOS, Windows and the BSDs. It replaces the older Go version of
mmdbio: all the old commands and options still work
([what changed](#upgrading-from-the-old-go-version)).

## Contents

- [Install](#install)
- [Words used in this guide](#words-used-in-this-guide)
- [A quick tour](#a-quick-tour)
- [The commands](#the-commands): read, metadata, stats, inspect, verify,
  export, diff, shrink, completion
- [Building your own MMDB file](#building-your-own-mmdb-file)
- [Using a config file](#using-a-config-file)
- [Record templates: records built from several CSV files](#record-templates-records-built-from-several-csv-files)
- [Using mmdbio from a C program](#using-mmdbio-from-a-c-program)
- [Upgrading from the old Go version](#upgrading-from-the-old-go-version)
- [For developers](#for-developers)
- [Credits and license](#credits-and-license)

## Install

Pick the line for your system:

| System | Command |
| --- | --- |
| Linux or macOS | `curl -fsSL https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.sh \| sh` |
| Windows (PowerShell) | `irm https://raw.githubusercontent.com/IPGeolocation/mmdbio/main/install.ps1 \| iex` |
| Debian or Ubuntu | download the `.deb` file from the [releases page](https://github.com/IPGeolocation/mmdbio/releases), then run `sudo apt install ./mmdbio_<version>_amd64.deb` |
| Fedora, RHEL or SUSE | download the `.rpm` file from the [releases page](https://github.com/IPGeolocation/mmdbio/releases), then run `sudo rpm -i mmdbio-<version>-1.x86_64.rpm` |
| Homebrew | use the formula in `packaging/homebrew/mmdbio.rb` |
| Anything else | [build it from source](#build-from-source) |

The install scripts download the right file for your computer, check that the
download is not damaged, and put `mmdbio` in `/usr/local/bin` (or in
`~/.local/bin` if you can't write there). On Windows it goes in
`%LOCALAPPDATA%\Programs\mmdbio` and is added to your PATH.

- To install a certain version, set `MMDBIO_VERSION`, for example
  `MMDBIO_VERSION=2.0.0`.
- To install into another folder, set `MMDBIO_DIR`.

Check that it works:

```bash
mmdbio --version
```

### Build from source

You only need a C compiler and `make`.

```bash
git clone https://github.com/IPGeolocation/mmdbio.git
cd mmdbio
make
sudo make install
```

- `make install` puts `mmdbio` in `/usr/local/bin`, plus the C library and
  its header.
- On FreeBSD, OpenBSD, NetBSD, DragonFly BSD and illumos, type `gmake`
  instead of `make`.
- `make test` runs the tests. It needs `python3`.
- On Linux the program is built as one self-contained file, so you can copy
  it to any Linux machine of the same type (for example, any 64-bit Intel or
  AMD machine).
- A 32-bit build cannot open files bigger than about 2 to 3 GB. Use a 64-bit
  build for big databases.

## Words used in this guide

| Word | Meaning |
| --- | --- |
| MMDB file | The database file. It usually ends in `.mmdb`. |
| IP block, network | A group of IP addresses that share the same information. It can be written in CIDR form, like `8.8.8.0/24`, which means every address from `8.8.8.0` to `8.8.8.255`. It can also be written as a range, with a first and a last address: `8.8.8.0-8.8.8.255`. |
| Record | The information stored for one network, for example `{"country": "US", "city": "Mountain View"}`. |
| Field | One item in a record, for example `country`. |
| Column | One column of a CSV file. When you build an MMDB file, each column usually becomes a field. |
| Header | The first line of a CSV file, when it holds the column names instead of data. |
| IPv4, IPv6 | The two kinds of IP address: `8.8.8.8` is IPv4, `2001:db8::1` is IPv6. One MMDB file can hold both. |

## A quick tour

This tour builds a small MMDB file and uses it. Every output shown here is
real.

Save this as `ips.csv`:

```csv
network,country,city,asn,is_vpn,score
8.8.8.0/24,US,Mountain View,15169,false,5
1.1.1.0/24,AU,Sydney,13335,false,3
2.56.0.0/16,DE,Berlin,64496,true,80
2001:db8::/32,NL,Amsterdam,64500,true,90
```

**1. Build the MMDB file.** You only need to give the input file:

```bash
mmdbio import ips.csv
```

```text
input: CSV (comma-delimited, with a header); network column "network"; 5 fields
rows: 4 read, 4 inserted, 0 skipped
tree: 4 unique records, 177 search-tree nodes, record size 24, 1,455 bytes
✅ Successfully wrote 4 entries to ips.mmdb
```

The first line tells you how mmdbio read your file: it found a header, and
it took the IP blocks from the `network` column. The new file is `ips.mmdb`.

**2. Look up an IP address:**

```bash
mmdbio read 8.8.8.8 ips.mmdb
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

**3. Export all VPN blocks as a list:**

```bash
mmdbio export ips.mmdb --where is_vpn=true --format cidr
```

```text
2.56.0.0/16
2001:db8::/32
Exported 2 networks as 2 CIDRs to stdout
```

**4. Check that the file is valid:**

```bash
mmdbio verify ips.mmdb
```

```text
valid
```

The rest of this guide explains each command in the same way.

## The commands

| Command | What it does |
| --- | --- |
| [`read`](#read-look-up-ip-addresses) | Looks up IP addresses. |
| [`metadata`](#metadata-information-about-the-file) | Shows information about the file: its name, size settings, build date and so on. |
| [`stats`](#stats-count-the-networks) | Shows the same information, plus how many networks the file holds. |
| [`inspect`](#inspect-list-the-fields) | Lists the fields of a record and their types. |
| [`verify`](#verify-check-that-a-file-is-valid) | Checks that a file is valid. |
| [`export`](#export-get-the-data-out) | Writes out the data, or a list of IP blocks. |
| [`diff`](#diff-compare-two-files) | Compares two files. |
| [`shrink`](#shrink-make-a-file-smaller) | Makes a file smaller without changing any answer. |
| [`import`](#building-your-own-mmdb-file) | Builds an MMDB file from a CSV or JSON file. |
| [`completion`](#completion-tab-completion) | Sets up Tab completion in your shell. |

A few things are the same for every command:

- You can give the files as plain arguments, or with flags: `--db` for the
  database, `--in` and `--out` for input and output, `--old` and `--new` for
  `diff`. Both ways work.
- `mmdbio <command> --help` lists every option of a command.
- Output is in colour only when it goes to your screen. `--nocolor` or
  `NO_COLOR=1` turns colour off.
- When something fails, mmdbio prints an error and exits with a non-zero
  status, so scripts can check for it.

### read: look up IP addresses

Give one or more addresses, then the database. **The database always comes
last.**

One address prints its record:

```bash
mmdbio read 8.8.8.8 ips.mmdb
```

Several addresses print one block of JSON, with `null` for an address that
has no data:

```bash
mmdbio read 8.8.8.8 1.1.1.1 9.9.9.9 ips.mmdb
```

```text
{
  "1.1.1.1": { "asn": "13335", "city": "Sydney", ... },
  "8.8.8.8": { "asn": "15169", "city": "Mountain View", ... },
  "9.9.9.9": null
}
```

You can also give:

- **a CIDR or a range** (`8.8.8.0/30`, `1.1.1.0-1.1.1.9`): every address in it
  is looked up, up to 2^32 addresses each;
- **a file of addresses**, one per line (lines starting with `#` are
  skipped);
- **`-`** to read addresses from standard input.

You can mix all of these in one command.

```bash
mmdbio read 2.56.0.0/30 ips.mmdb -f csv
```

```text
ip,asn,city,country,is_vpn,score
2.56.0.0,64496,Berlin,DE,true,80
2.56.0.1,64496,Berlin,DE,true,80
2.56.0.2,64496,Berlin,DE,true,80
2.56.0.3,64496,Berlin,DE,true,80
```

| Option | What it does |
| --- | --- |
| `-f json` | The default. One block of JSON, keyed by IP address. A single address prints its record alone. |
| `-f jsonl` | One line of JSON per address that has data, with the address in an `"ip"` field. |
| `-f json-pretty` | Like `jsonl`, but spread over several lines for reading. |
| `-f csv`, `-f tsv` | A header line (`ip`, then the fields), then one line per address that has data. |
| `--fields a,b` | Show only these fields. Use dots for nested fields: `location.city.name`. |
| `--flatten` | For CSV and TSV: put every nested field in its own column, instead of putting nested data in one column as JSON. |
| `--no-header` | For CSV and TSV: leave out the header line. |
| `--out file` | Write to a file instead of the screen. |
| `--ip`, `--range`, `--input` | The old way to give an address, a range, or a file of addresses. These still work. |

### metadata: information about the file

```bash
mmdbio metadata ips.mmdb
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

The database type and description come from `import`, which sets them with
`--title` and `--description`.

| Option | What it does |
| --- | --- |
| `--json` (or `-f json`) | Print everything as JSON. |
| `--layout` (`-l`) | Also show the format version, and the size and position of each part of the file. |
| `--data-types` | Also show how much of the data is text, numbers, lists and so on. This reads the whole file. |

### stats: count the networks

`stats` prints the same information as `metadata`, then counts the networks:

```bash
mmdbio stats ips.mmdb
```

```text
IPv4 Count:    3
IPv6 Count:    1
Networks:      4
```

Add `--json` to get JSON.

### inspect: list the fields

`inspect` looks at the record of one address and lists its fields and their
types:

```bash
mmdbio inspect ips.mmdb --sample-ip 8.8.8.8
```

```text
asn      : string
city     : string
country  : string
is_vpn   : string
score    : string
```

`--out schema.json` saves the list as JSON.

### verify: check that a file is valid

```bash
mmdbio verify ips.mmdb
```

If the file is good, it prints `valid` and exits with status 0. If not, it
prints `invalid:` and the reason, and exits with status 1:

```bash
mmdbio verify notes.txt
```

```text
invalid: error opening database: invalid MMDB file
```

Run `verify` after you build a file and before you ship it.

### export: get the data out

`export` writes every network and its record. With no output file it writes
JSON to the screen:

```bash
mmdbio export ips.mmdb
```

```text
{
  "1.1.1.0/24": { "asn": "13335", "city": "Sydney", ... },
  "2.56.0.0/16": { ... },
  "8.8.8.0/24": { ... },
  "2001:db8::/32": { ... }
}
Exported 4 records to stdout
```

Give an output file, and its ending picks the format: `.json`, `.jsonl`,
`.csv` or `.tsv`.

```bash
mmdbio export ips.mmdb out.csv
```

```text
range,asn,city,country,is_vpn,score
1.1.1.0/24,13335,Sydney,AU,false,3
2.56.0.0/16,64496,Berlin,DE,true,80
8.8.8.0/24,15169,Mountain View,US,false,5
2001:db8::/32,64500,Amsterdam,NL,true,90
```

**Filter with `--where`.** Only networks whose record matches are written.
You can repeat `--where`, and then every condition must match.

```bash
mmdbio export ips.mmdb --where is_vpn=true --format cidr
```

```text
2.56.0.0/16
2001:db8::/32
```

| In `--where` | Meaning | Example |
| --- | --- | --- |
| `=` | equals one of the values (separate several with commas) | `country=DE,FR` |
| `!=` | equals none of the values | `country!=US` |
| `>`, `>=`, `<`, `<=` | number comparison | `score>=50` |

- **Put the condition in single quotes when it has `>`, `<` or a space**, for
  example `--where 'score>=50'`. Without the quotes, your shell reads `>` as
  "write to a file", and the filter never reaches mmdbio.
- Text is compared without caring about upper or lower case.
- If the field holds a list, the condition matches when any item matches.
- A record that does not have the field never matches.
- For nested fields, use dots: `location.country.code2=DE`.

```bash
mmdbio export ips.mmdb -f jsonl --where 'score>=50' --fields city,score
```

```text
{"city":"Berlin","range":"2.56.0.0/16","score":"80"}
{"city":"Amsterdam","range":"2001:db8::/32","score":"90"}
```

| Option | What it does |
| --- | --- |
| `-f`, `--format` | `json` (one block keyed by network), `jsonl` (one line per network, with a `"range"` field), `csv`, `tsv`, or `cidr` (just the networks, one per line). Without it, the output file's ending decides, and the screen gets `json`. |
| `--out file` | The output file. `-` or no output means the screen; the count line then goes to standard error, so it does not mix with the data. |
| `--where` | Keep only matching networks (see above). |
| `--fields a,b` | Keep only these fields. |
| `--family 4` or `6` | Only IPv4 or only IPv6 networks. |
| `--range 8.8.0.0/16,1.1.1.0/24` | Only networks inside these blocks. |
| `--flatten`, `--no-header` | For CSV and TSV, the same as in `read`. |
| `--aggregate` | With `--format cidr`, join neighbouring networks into as few blocks as possible. It is on by default; `--aggregate=false` lists each network as it is stored. |

CSV and TSV values are put in quotes only when they need it, so `mmdbio
import` can read every export back.

### diff: compare two files

Give the old file first and the new file second. `diff` lists the networks
that were added, removed or changed:

```bash
mmdbio diff ips.mmdb ips-new.mmdb
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

`--summary` prints only the counts. Two files with the same data print
`Added: 0 | Removed: 0 | Modified: 0`.

```bash
mmdbio diff ips.mmdb ips-new.mmdb --summary
```

```text
Added: 2 | Removed: 2 | Modified: 1
```

The plain `diff` compares networks by their exact size. When a block was
split or joined (for example `/16` became `/17`), these two options say what
really changed:

- `-s` (`--subnets`): shows each network whose edges moved, as `old -> new`.
- `-r` (`--records`): shows each network that kept its edges but got a
  different record, with the old record (`-`) and the new one (`+`).

```bash
mmdbio diff ips.mmdb ips-new.mmdb -s -r
```

```text
Subnets changed: 3
   2.56.0.0/16 -> 2.56.0.0/17
   9.0.0.0/8 (no data) -> 9.9.9.0/24
   2001:db8::/32 -> 2000::/3 (no data)

Records changed: 1
   1.1.1.0/24
      - {"asn":"13335","city":"Sydney",...}
      + {"asn":"13335","city":"Melbourne",...}
```

`--json` prints the result as JSON, with the full old and new records. If
either file can't be read, `diff` prints a warning and exits with status 1.

### shrink: make a file smaller

Many files repeat the same parts over and over. `shrink` stores each repeated
part once, and drops data that nothing points to. The new file gives exactly
the same answer for every IP address, and any program that reads MMDB files
can open it.

```bash
mmdbio shrink city.mmdb
```

This writes `city.shrunk.mmdb`. To see how much you would save, without
writing anything:

```bash
mmdbio shrink city.mmdb --dry-run
```

**Before the new file is kept, mmdbio checks it.** Both files must have the
same metadata (only the size settings may differ), and both must give the
same answer for:

- a fixed list of 41 edge addresses: the edges of the IPv4 address classes
  and of the private and special-use blocks (such as `10.0.0.0` and
  `192.168.255.255`), and of IPv6's special blocks (such as `::1`, `fc00::`
  and `fe80::`);
- 1,000,000 random IPv4 addresses and 1,000,000 random IPv6 addresses.

If any answer differs, nothing is written.

| Exit status | Meaning |
| --- | --- |
| 0 | Done, and every check passed. |
| 2 | A lookup gave a different answer. Nothing was written. |
| 4 | The metadata is different. Nothing was written. |
| 1 | Any other error. |

| Option | What it does |
| --- | --- |
| `--out file` | The output file (default: `<name>.shrunk.mmdb`). |
| `--overwrite` | Replace the output file if it already exists. |
| `--dry-run` | Only report the savings. |
| `--record-size 24`, `28` or `32` | Change how much room each link in the file takes. Smaller is smaller on disk; if it is too small for your file, mmdbio says which size fits. |
| `--full-check` | Also compare every network of both files. This is the strongest check, and the slowest. |
| `--lookups N`, `--seed N` | How many random addresses to check, and which ones (the same seed always checks the same addresses). |
| `--check=false` | Skip the checks. |
| `--no-compact` | Copy the data as it is, without removing data that nothing points to. |
| `--bench=false` | Skip timing how fast lookups are in both files. |
| `--bench-rounds N` | Time the lookups N times and report the middle result. Use 3 or more for steadier numbers. |
| `--full-report` | Also measure how much memory a program uses to read each file (Linux and macOS). |
| `-v` | Show each step and how long it took. |

How much a file shrinks depends on how much of it repeats. Big files with
few different records shrink the most, and a shrunk file does not shrink
again. `shrink` reads the input without loading it into memory; it needs
roughly 30 bytes of memory for each distinct part it keeps.

The way `shrink` works and checks its result follows IPinfo's
[`mmdbshrink`](https://github.com/ipinfo/mmdbctl/tree/master/mmdbshrink).
mmdbio's version is written separately, in C.

### completion: Tab completion

```bash
mmdbio completion install
```

Open a new terminal. Now the Tab key completes commands, options, option
values and `.mmdb` file names. It works in bash, zsh, fish and PowerShell.

- `mmdbio completion uninstall` removes it.
- `mmdbio completion bash` (or `zsh`, `fish`, `powershell`) prints the
  completion script, if you'd rather install it yourself.

## Building your own MMDB file

`mmdbio import` builds an MMDB file from a CSV, TSV, JSON or NDJSON file. The
file can be gzipped (`.gz`). **The only thing you must give is the input
file.** mmdbio works out the rest, and every setting can be changed with an
option or in a [config file](#using-a-config-file).

```bash
mmdbio import ips.csv
```

This writes `ips.mmdb` next to the input. To pick the output name, add it as
a second argument: `mmdbio import ips.csv city.mmdb`.

After every build, mmdbio prints a short report. **Always read the first
line.** It says how mmdbio read your file:

```text
input: CSV (comma-delimited, with a header); network column "network"; 5 fields
rows: 4 read, 4 inserted, 0 skipped
```

### How mmdbio reads a CSV file

mmdbio answers three questions about your file. Each answer can be changed.

| Question | How mmdbio decides | How to change it |
| --- | --- | --- |
| What separates the columns? | It looks at the first line: comma, tab, semicolon or pipe. | `--delimiter ';'` |
| Is the first line a header? | Yes, unless that line holds an IP address, a CIDR or a range, or its first value is only digits. | `--header true` or `--header false` |
| Which column holds the IP blocks? | It looks for a known column name. Without a header, it looks at the first columns. | `--network-column`, or `--start-column` and `--end-column` |

Every column that does not hold the IP block becomes a field of the record,
with the column name as the field name. **Values are stored as text**,
unless you [give a type](#types-numbers-true-or-false-and-lists).

### Files with a header

Name your IP column (or columns) with one of these names. Upper and lower
case, `_`, `-` and spaces are ignored, so `start_ip`, `StartIP`, `Start IP`
and `start-ip` all work.

| Your file has | Use one of these names |
| --- | --- |
| **One column** with the IP block | `network`, `cidr`, `prefix`, `subnet`, `ip_range`, `range`, `ip_network`, `net`, `block`, `netblock`, `ip`, `ip_address`, `address`, `ips`, `ip_range_cidr` |
| **Two columns**: the first address... | `start_ip`, `ip_start`, `start`, `from`, `ip_from`, `from_ip`, `first_ip`, `ip_first`, `range_start`, `start_address`, `begin_ip`, `network_start_ip`, `start_ip_address`, `low_ip`, `ip_low`, `min_ip` |
| ...and the last address | `end_ip`, `ip_end`, `end`, `to`, `ip_to`, `to_ip`, `last_ip`, `ip_last`, `range_end`, `end_address`, `network_last_ip`, `end_ip_address`, `high_ip`, `ip_high`, `max_ip`, `stop_ip` |

For example:

| Header | mmdbio takes the IPs from |
| --- | --- |
| `network,country` | the `network` column |
| `cidr,country` | the `cidr` column |
| `ip,country` | the `ip` column |
| `start_ip,end_ip,country` | the `start_ip` and `end_ip` columns |
| `ip_from,ip_to,country` | the `ip_from` and `ip_to` columns |
| `Start IP,End IP,Country` | the `Start IP` and `End IP` columns |

**If your column has another name**, mmdbio stops with this message:

```text
Error: cannot tell which column holds the IP networks; name it with --network-column, or --start-column and --end-column (or "network" in the config)
```

Tell it the name:

```bash
mmdbio import file.csv --network-column addr
```

or, for two columns:

```bash
mmdbio import file.csv --start-column first --end-column last
```

### What the IP column can hold

| Form | Example | Notes |
| --- | --- | --- |
| CIDR | `8.8.8.0/24`, `2001:db8::/32` | |
| One address | `8.8.8.8` | Means just that address. |
| A range in one column | `8.8.8.0-8.8.8.255` | |
| A number | `134744064` | The address as one number. Up to 4294967295 is IPv4. |

The start and end columns each take an address or a number. IPv4 and IPv6
rows can be mixed in one file. Rows can come in any order.

### Files without a header

When the first line is data, the columns are named `column1`, `column2`, and
so on. mmdbio then looks for the IP block by position:

| The first line has | mmdbio uses |
| --- | --- |
| an IP block in the first column | the first column |
| an address in the first column and an address in the second column | the first two columns, as first and last address |
| the IP block in any other column | nothing. It stops with an error, and you must say which column it is. |

For example, a file with the IP block in the second column:

```text
US,8.8.8.0/24
AU,1.1.1.0/24
```

```bash
mmdbio import file.csv --network-column column2
```

The records then look like `{"column1": "US"}`. To give the columns real
names, use `--columns`, which names the columns in order:

```bash
mmdbio import file.csv --columns country,network
```

Now the records look like `{"country": "US"}`. Because one column is now
called `network`, mmdbio finds it on its own.

If your first data line has no IP address in it, mmdbio would think it is a
header. Use `--header false` to tell it there is no header.

### Files with more than one IP column

Some files have the same IP block in several forms, for example `cidr`,
`start_ip` and `end_ip`. mmdbio follows these rules:

1. A first-and-last pair (`start_ip`, `end_ip`) is used before a single
   column (`cidr`).
2. Among single columns, the name higher in the table above is used
   (`network` before `cidr`, `cidr` before `ip`).
3. **IP columns that are not used are stored in the record like any other
   column.**

So for this file:

```text
cidr,start_ip,end_ip,country
8.8.8.0/24,8.8.8.0,8.8.8.255,US
```

| Command | IPs taken from | The record |
| --- | --- | --- |
| `mmdbio import file.csv` | `start_ip` and `end_ip` | `{"cidr": "8.8.8.0/24", "country": "US"}` |
| `mmdbio import file.csv --exclude cidr` | `start_ip` and `end_ip` | `{"country": "US"}` |
| `mmdbio import file.csv --network-column cidr --exclude start_ip,end_ip` | `cidr` | `{"country": "US"}` |

**Tip:** when your file has more than one IP column, say which one to use,
and leave the others out with `--exclude`.

### Types: numbers, true or false, and lists

CSV values are stored as text. Use `--type` to store a column as something
else:

```bash
mmdbio import ips.csv --type asn=uint32 --type is_vpn=boolean --type score=uint16
```

Now `8.8.8.8` gives `{"asn": 15169, "is_vpn": false, "score": 5, ...}`
instead of `{"asn": "15169", "is_vpn": "false", "score": "5", ...}`.

| Type | Stores | From a CSV value like |
| --- | --- | --- |
| `string` | text (the default) | anything |
| `uint16` | a whole number from 0 to 65,535 | `443` |
| `uint32` | a whole number from 0 to 4,294,967,295 | `15169` |
| `uint64` | a whole number from 0 to 2^64 - 1 | `18446744073709551615` |
| `uint128` | a whole number from 0 to 2^128 - 1 | a very large number |
| `int32` | a whole number from -2,147,483,648 to 2,147,483,647 | `-5` |
| `double`, `float` | a decimal number (`double` keeps more digits) | `48.137` |
| `boolean` | true or false | `true`/`false`, `yes`/`no`, `t`/`f`, `y`/`n`, `1`/`0`, in any case |
| `array` | a list of text items | `"[dns,google]"` or `dns,google` |
| `auto` | guessed from each value | numbers become numbers (`12`, `4.5`), `true`/`false` becomes true/false, everything else stays text (`007` stays text) |

- An empty value in a number or true/false column leaves the field out of
  that record.
- A value that doesn't fit its type (for example `abc` in a `uint16` column)
  makes the row bad, and it is skipped with a warning.
- `--infer-types` uses `auto` for every column.
- `--array-separator ';'` changes the character that splits list items (the
  default is a comma).

### Overlapping rows

Two rows can cover the same addresses, for example `8.8.0.0/16` and
`8.8.8.0/24`. By default **the later row in the file wins**. `--merge`
changes this:

| `--merge` | What happens where rows overlap |
| --- | --- |
| `none` (or `replace`; the default) | the later row wins |
| `keep` | the earlier row wins |
| `toplevel` | the two records are combined; for a field in both, the later row wins |
| `recurse` | like `toplevel`, but nested records and lists are combined too |

`--order ipv6-first` treats every IPv4 row as later than every IPv6 row, as
if the file listed all its IPv6 rows first. Use it when an IPv6 row covers
IPv4 space and the IPv4 rows should win.

### Bad rows and empty values

- **A bad row** (for example, an IP column that holds `not-an-ip`) is skipped
  with a warning that gives its line number. The first 20 warnings are
  shown, then a count. `--strict` stops the build at the first bad row
  instead.

  ```text
  warn: file.csv:3: invalid network "not-an-ip"
  rows: 3 read, 2 inserted, 1 skipped
  ```

- **An empty value** is stored as `""`. With `--omit-empty`, the field is
  left out of that record.
- `--empty-if country=-` treats the value `-` in the `country` column as
  empty. You can repeat it, and give one column several values.
- **Invalid text** (bytes that are not valid UTF-8) is replaced with the `�`
  character, so the file always passes `verify`.

### Choosing and naming fields

| Option | What it does |
| --- | --- |
| `--include a,b` | Only these columns become fields. |
| `--exclude a,b` | These columns are left out. |
| `--rename old=new` | Store a column under another name. You can repeat it. |
| `--nest-keys` | Turn dotted column names into nested records: the columns `location.country` and `location.city` become `{"location": {"country": ..., "city": ...}}`. |
| `--network-field` | Add a `network` field to each record, holding the row's CIDR (or its range as `first-last`). |
| `--no-fields` | Store no columns at all. Use this to turn a plain list of IPs into a database that only says "this IP is in the list". |
| `--fill-gaps` | Give every address that no row covers a record with every field set to `""`. Rows always win over this filler. |

`--include`, `--exclude`, `--type` and `--empty-if` use the input's column
names. `--rename` decides the name the field gets.

### JSON input

mmdbio reads three kinds of JSON. It looks for the IP block under the same
names as CSV columns (`network`, `cidr`, `start_ip` and so on).

| Shape | Example |
| --- | --- |
| One object, with the networks as keys | `{"8.8.8.0/24": {"country": "US", "asn": 15169}}` |
| A list of objects | `[{"network": "8.8.8.0/24", "country": "US"}]` |
| One object per line (NDJSON, `.jsonl`) | `{"start_ip": "8.8.8.0", "end_ip": "8.8.8.255", "country": "US"}` |

JSON keeps its own types: whole numbers stay numbers, `true` and `false` stay
true and false, `null` becomes `""`, and nested objects and lists stay
nested. You can still use `--type`, with dots for nested fields
(`location.latitude=double`) and `[]` for list items (`tags[]=uint16`).

### Lookup files: records from a second CSV

Sometimes the IP file only holds an id, and the details are in another file:

```text
ranges.csv:       start_ip,end_ip,asn_id
asn-details.csv:  id,asn,name,country
```

`--lookup` builds each record from the matching row of the second file:

```bash
mmdbio import ranges.csv asn.mmdb --lookup asn-details.csv --lookup-key id --lookup-on asn_id
```

- `--lookup-key` is the id column of the lookup file (default: its first
  column).
- `--lookup-on` is the id column of the IP file (default: the same name as
  `--lookup-key`).
- If an id appears twice in the lookup file, its last row is used.
- An id that is missing from the lookup file gets a record with every field
  set to `""`, and the report counts these.
- The lookup file is not loaded into memory, so it can be very large.

For records built from several files, or nested records, use a
[record template](#record-templates-records-built-from-several-csv-files).

### The settings of the file you build

| Option | What it sets | Default |
| --- | --- | --- |
| `--out file` (or the second argument) | the output file; `-` writes to standard output | the input name, with `.mmdb` |
| `--title` | the database type, a short name stored in the file | the input file's name |
| `--description` | a description | `Custom IP Intelligence Database` |
| `--languages en,de` | the languages the file says it holds | `en` |
| `--ip 4` or `6` | IPv4-only, or both IPv4 and IPv6 | `6`, so the file answers both |
| `--size 24`, `28`, `32` or `auto` | how much room each link in the file takes | `auto`, the smallest that fits |
| `--alias-6to4` | makes IPv6 addresses that contain an IPv4 address (`::ffff:8.8.8.8`, and the `2001::/32` and `2002::/16` blocks) give the IPv4 answer | off |
| `--disallow-reserved` | drops data for private and special-use blocks, like `10.0.0.0/8` and `192.168.0.0/16` | off: data for them is kept |
| `--shrink` | shrinks and checks the file after building it, as `mmdbio shrink` does | off |

### Big files

mmdbio does not hold the whole file in memory. It sorts rows in memory up to
a limit, writes the rest to temporary files, and writes the database to disk
as it goes. So the size of your input is not limited by your memory.

| Option | What it does | Default |
| --- | --- | --- |
| `--memory MB` | the memory used for sorting rows | 512 MB |
| `--tmp-dir folder` | where temporary files go | `$TMPDIR`, else the system's temporary folder |

Put `--tmp-dir` on a fast local disk with plenty of free space. The new
database is first written under a temporary name, and gets its real name only
when the build succeeds, so a failed build never replaces a good file.

`-q` (`--quiet`) prints only errors and the last line.

### If you used mmdbctl before

mmdbio also accepts the import options of IPinfo's `mmdbctl`, so its import
commands run unchanged: `--csv`, `--tsv`, `-j` (`--json`), `--fields` (`-f`,
names for the columns after the IP columns, in a file without a header),
`--fields-from-header`, `--range-multicol`, `--joinkey-col`, `--no-network`,
`--ignore-empty-values` (the same as `--fill-gaps`), and
`--disable-metadata-pointers`.

Two short options are different: in mmdbio, `-c` means `--config` and `-t`
means `--title`. Also note that in `import`, `-f` means `--fields`, while in
`read` and `export` it means `--format`.

## Using a config file

Instead of many options, you can put the settings in a JSON file:

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
```

- **Options on the command line win over the config file**, and the config
  file wins over the defaults. So `mmdbio import --config build.json --size
  32` uses everything from the file, but a record size of 32.
- File paths in the config are taken from the config file's folder.
- A key that mmdbio doesn't know is an error. This catches typing mistakes.
- Keys that start with `_` are ignored, so you can use them for notes:
  `"_comment": "built every Monday"`.
- `examples/import-config.json` shows most keys.

### Config keys and their options

| Config key | Same as option | Value |
| --- | --- | --- |
| `input`, or `input.path` | `--in` | the input file (`-` is standard input) |
| `input.format` | `--format` | `auto`, `csv`, `tsv`, `json` or `jsonl` |
| `input.delimiter` | `--delimiter` | one character, or `comma`, `tab`, `semicolon`, `pipe`, `space` |
| `input.header` | `--header` | `true`, `false` or `"auto"` |
| `input.columns` | `--columns` | `["network", "country", "city"]` |
| `input.lazy_quotes` | `--lazy-quotes` | `true` to accept stray quote marks inside CSV values |
| `output` | `--out` | the output file |
| `network`, or `network.column` | `--network-column` | the IP column, for example `"cidr"` |
| `network.start_column`, `network.end_column` | `--start-column`, `--end-column` | the first and last address columns |
| `lookup`, or `lookup.path` | `--lookup` | the lookup file |
| `lookup.key_column`, `lookup.input_column` | `--lookup-key`, `--lookup-on` | the id columns |
| `metadata.database_type` | `--title` | text |
| `metadata.description` | `--description` | text, or one per language: `{"en": "...", "de": "..."}` |
| `metadata.languages` | `--languages` | `["en", "de"]`; `[]` stores none |
| `metadata.ip_version` | `--ip` | `4` or `6` |
| `metadata.record_size` | `--size` | `24`, `28`, `32` or `"auto"` |
| `tree.alias_ipv4` | `--alias-6to4` | `true` or `false` |
| `tree.include_reserved` | the opposite of `--disallow-reserved` | `true` keeps data for private and special-use blocks |
| `tree.fill_gaps` | `--fill-gaps` | `true` or `false` |
| `merge` | `--merge` | `replace`, `keep`, `toplevel` or `recurse` |
| `order` | `--order` | `file` or `ipv6-first` |
| `on_error` | `--strict` | `skip` (the default) or `fail` |
| `fields.include`, `fields.exclude` | `--include`, `--exclude` | `["a", "b"]` |
| `fields.types` | `--type` | `{"score": "uint16", "is_vpn": "boolean"}` |
| `fields.infer_types` | `--infer-types` | `true` or `false` |
| `fields.nest_keys` | `--nest-keys` | `true` or `false` |
| `fields.empty` | `--omit-empty` | `keep` or `omit` |
| `fields.array_separator` | `--array-separator` | one character |
| `fields.rename` | `--rename` | `{"org": "organization"}` |
| `fields.empty_if` | `--empty-if` | `{"asn": "0", "country": ["", "-"]}` |
| `fields.network_field` | `--network-field` | `true` or `false` |
| `fields.no_fields` | `--no-fields` | `true` or `false` |
| `shrink` | `--shrink` | `true` or `false` |
| `temp_dir` | `--tmp-dir` | a folder |
| `memory_mb` | `--memory` | megabytes |
| `vars` | `--var name=value` | values for `${name}` in paths (see below) |
| `record` | | a [record template](#record-templates-records-built-from-several-csv-files) |

### Three examples

**A file without a header, with the IP block in the second column:**

```json
{
  "input": {"path": "ranges.csv", "header": false, "columns": ["country", "network"]},
  "output": "ranges.mmdb",
  "network": "network"
}
```

**A file with `cidr`, `start_ip` and `end_ip`, using `cidr`:**

```json
{
  "input": "ranges.csv",
  "output": "ranges.mmdb",
  "network": {"column": "cidr"},
  "fields": {"exclude": ["start_ip", "end_ip"]}
}
```

**Column names with spaces, a renamed field, and a stop at the first bad
row:**

```json
{
  "input": {"path": "ranges.csv", "format": "csv", "delimiter": ",", "header": true},
  "output": "ranges.mmdb",
  "network": {"start_column": "Start IP", "end_column": "End IP"},
  "fields": {"rename": {"Country": "country"}},
  "on_error": "fail"
}
```

### One config for several folders

Paths can hold `${name}`, which is filled in from `vars`, and can start with
`~` for your home folder:

```json
{
  "vars": {"root": "~/data"},
  "input": "${root}/ranges.csv",
  "output": "${root}/ranges.mmdb"
}
```

`--var` changes a value for one run, so the same config builds from another
folder:

```bash
mmdbio import --config build.json --var root=/data/samples
```

A name that is not in `vars` and not given with `--var` is taken from the
environment, so `${HOME}` works too. A file named `data.csv` that only exists
as `data.csv.gz` is read from the `.gz`.

## Record templates: records built from several CSV files

Use a record template when each record needs data from more than one CSV
file, or needs nested parts. For example: an IP file that holds a country id
and a city id, a country file, and a file of place names in several
languages.

A template goes under `record` in the config file. It describes the shape of
the record, and says where each value comes from. The input must be CSV or
TSV. With a template, the `fields` and `lookup` settings are not used.

### An example

The files:

```text
ranges.csv:     start_ip,end_ip,country_id,city_name_id,latitude,radius,time_zone
countries.csv:  id,iso_code,name_id
names.csv:      id,name_en,name_de
```

The config:

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

A row of `ranges.csv` becomes a record like this:

```json
{"country": {"iso_code": "DE", "names": {"en": "Germany", "de": "Deutschland"}},
 "city": {"names": {"en": "Munich", "de": "München"}},
 "location": {"latitude": 48.137, "accuracy_radius": 20},
 "time_zone": "Europe/Berlin"}
```

Here is how to read it:

- `tables` names the extra CSV files. Each one is looked up by its `key`
  column (the first column if you don't say).
- `shape` is the record. A plain text value, like `"time_zone"`, means "the
  value of this column".
- `{"$table": "countries", "$on": "country_id", ...}` means: take the
  `country_id` of the current row, find the row with that id in
  `countries.csv`, and read the columns inside this part from that row. Inside
  it, `"$on": "name_id"` does the same again, one level deeper, in
  `names.csv`.
- `define` holds a part you use more than once. `{"$use": "localized"}` puts
  it in.
- `skip_if` leaves out rows whose country code is empty or `ZZ`.
- `{"$col": "radius", "$ceil": true}` takes the `radius` column and rounds it
  up to a whole number.
- `types` gives a type to every value stored under that name, wherever it is.

### The parts of `record`

| Key | What it does |
| --- | --- |
| `shape` | The record (required). It must be a map. |
| `tables` | The extra CSV files: `"name": "path"`, or `"name": {"path": "...", "key": "id", "trim": false}`. If an id appears twice, its last row is used. Tables are not loaded into memory. |
| `define` | Named parts to reuse with `$use`. |
| `types` | `"field": "type"`: the type of every value stored under that name. Without it, values are text. The types are the same as for `--type`. |
| `trim` | `true` removes spaces (and other blank characters, like non-breaking spaces) from both ends of every value. The default is `false`. |
| `empty` | `keep` (the default) stores empty values as `""`. `omit` leaves them out, and also leaves out maps and lists that end up empty. |
| `invalid` | What happens when a value breaks a rule (see the steps below): `bad` (the default) skips the row with a warning; `omit` leaves the value out and counts it in the report; `skip` leaves the row out without a warning. |
| `skip_if` | `[{"value": ..., "in": ["", "ZZ"]}]`: a row whose value is in the list is left out. |
| `skip_empty` | `true` leaves out rows whose record ends up empty. |

### What you can write in the shape

| Write | It means |
| --- | --- |
| `"column"` | The value of that column. If the file has no such column, that's an error, unless you write `"column?"`, which reads as empty. |
| `{"key": ..., ...}` | A map (a nested record). |
| `[..., ...]` or `{"$array": [...]}` | A list. Values that are left out are dropped from the list. |
| `12`, `true`, or `{"$const": ...}` | A fixed value. A text `$const` goes through the steps below. |
| `{"$col": "column", ...steps}` | A column, with steps applied to its value. |
| `{"$table": "t", "$on": "column", ...}` | Read this part from the row of table `t` whose id is in `column` of the current row. These can be nested. |
| `"$missing"` (next to `$table`) | What to do when the id is not in the table (an empty id is never found): `empty` (the default: every column reads as empty), `omit` (leave this part out), or `skip` (leave the whole row out). |
| `{"$use": "name"}` | Put in the part from `define`. |
| `{"$first": [..., ...]}` | The first value that is not empty. |
| `"$trim"`, `"$empty"`, `"$invalid"` | Change the record's `trim`, `empty` or `invalid` setting for this part and everything inside it. |

### Steps

Steps change or check a value. You add them next to `$col` (or a text
`$const`), and they run in this order:

| Step | What it does |
| --- | --- |
| `"$trim": true` | Remove blank characters from both ends. |
| `"$case": "upper"` or `"lower"` | Change the case of the letters A to Z. |
| `"$empty_if": ["0", "-"]` | Treat these values as empty. |
| `"$strip_prefix": {"equals": ..., "sep": "-"}` | Remove a prefix: `US-CA` becomes `CA` when the other value is `US`. If the prefix is different, the value becomes empty. |
| `"$one_of": ["a", "b"]` | Only these values are allowed. |
| `"$chars": "A-Z0-9"` | Only these characters are allowed. |
| `"$length": [2, 3]` | The value must have between 2 and 3 characters. |
| `"$number": [-90, 90]` | The value must be a number between -90 and 90. |
| `"$ceil": true` | Round up to a whole number. |
| `"$default": "..."` | Use this value when the value is empty. |
| `"$parse": "literal"` | Read a list or a map written as text, in Python, Perl or JSON style: `['a', 'b']`, `[{'k': 'v'}]`, `{"k": 1}`. `True`, `None` and `\xNN` codes are understood. |
| `"$parse": "list"` | Read `[A, B, C]` as a list: split on commas, remove spaces and quote marks around each item. |
| `"$type": "uint16"` | The type of this value. It wins over `types`. |

The checking steps (`$one_of`, `$chars`, `$length`, `$number`) let empty
values through. When a value fails a check, the record's `invalid` setting
decides what happens. The report at the end counts the ids that a table did
not have, and the values left out because they failed a check.

### Speed and memory with templates

- If two rows have the same values in the columns the template reads, the
  second row reuses the first row's record instead of building it again. For
  example, when millions of IP blocks point to a much smaller number of
  companies, each company's record is built once. (This does not apply with
  `--network-field`, `--no-fields` or `--fill-gaps`, which make each record
  different.)
- A part read from a table (`$table`) is also built only once per id. When
  that part is large (1 KB or more), it is also stored only once while the
  file is being built, and each record points to it. For example, a large
  company record with long lists of addresses is stored once, not once for
  every IP block that uses it. The finished file is exactly the same either
  way; this only saves memory and disk space during the build.

## Using mmdbio from a C program

mmdbio also comes as a C library. `make lib` builds `build/libmmdbio.a`, and
`include/mmdbio.h` describes it. Only names that start with `mmdbio_` are
visible to your program.

| Function | What it does |
| --- | --- |
| `mmdbio_open`, `mmdbio_close`, `mmdbio_get_metadata` | Open a database and read its information. |
| `mmdbio_lookup` | Look up an address. You get the record as JSON, and the network it belongs to. |
| `mmdbio_networks` | Go through every network that has data, in order. |
| `mmdbio_verify` | The same checks as `mmdbio verify`. |
| `mmdbio_shrink`, `mmdbio_check` | Shrink a file; check that two files give the same answers. |
| `mmdbio_bench`, `mmdbio_bench_paired` | Time lookups in one file, or in two files side by side. |
| `mmdbio_build` | Build a database from an import config file. |

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

Build it with:

```bash
cc -Iinclude my.c build/libmmdbio.a -lm
```

On Windows, add `-lpsapi`. `examples/lib_example.c` is a small working
example, and `tests/lib_test.c` uses every function.

## Upgrading from the old Go version

This version replaces the Go version of mmdbio (1.x). All its commands,
options and output formats still work, so your scripts keep working. If you
installed the Go version with `go install`, switch to one of the
[install commands](#install) above.

What changed:

- **`import` reads more kinds of files**: CSV, TSV, JSON lists and NDJSON,
  gzipped or not, as well as the old JSON format. It also takes a config
  file. The old options still work.
- **`import` follows the order of your file.** The Go version took rows in a
  random order, so overlapping rows could give a different result on each
  run. Now the later row always wins (or the earlier one, with `--merge
  keep`, which is new).
- **Numbers in JSON stay whole numbers.** The Go version stored every number
  as a decimal number. Now whole numbers are stored as whole numbers, and
  `--type` can set any type.
- **Some defaults changed.** `--size` is now `auto` (it was 32), and
  `--title` is now the input file's name (it was `Custom-ip-database`).
- **`export` lists networks in address order** instead of sorted as text. The
  data is the same. It also uses little memory, even on very large files.
- **`diff` and `stats` count each network once.** The Go version also counted
  the IPv4 copies inside IPv6 files, and showed IPv4 networks of an IPv6 file
  in IPv6 form (`::102:300/120`). Now IPv4 networks are shown as IPv4.
- **`inspect` lists fields in alphabetical order** (it was random).
- **`verify` now exists.** It was described before, but missing.
- **Tab completion** completes more: option values and `.mmdb` file names.
  `completion install` sets it up for you.
- **`export` without an output file** writes to the screen (it used to be an
  error).
- **New:** `shrink`, CSV/TSV/JSON Lines output for `read` and `export`, and
  files given as plain arguments.

## For developers

### Cross-compiling

From a Linux machine you can build for other systems:

| Target | Command |
| --- | --- |
| Windows (64-bit) | `make CC=x86_64-w64-mingw32-gcc BUILD=build-windows` |
| Windows (32-bit) | `make CC="clang --target=i686-w64-mingw32" BUILD=build-windows-386 LIBAR=i686-w64-mingw32-ar NM=i686-w64-mingw32-nm OBJCOPY=i686-w64-mingw32-objcopy` |
| Linux, 32-bit Intel/AMD | `make CC=i686-linux-gnu-gcc BUILD=build-386`, or `CC="gcc -m32"` with gcc-multilib. Needs GCC 14 or newer. On Debian and Ubuntu, gcc-multilib can't be installed next to the ARM cross compilers, but gcc-i686-linux-gnu can. |
| Linux, 64-bit ARM | `make CC=aarch64-linux-gnu-gcc BUILD=build-arm64` |
| Linux, 32-bit ARM | `make CC="clang --target=armv7-linux-gnueabihf" BUILD=build-armv7`. GCC can't build this one. With clang older than 21, also add `--ld-path=$(command -v arm-linux-gnueabihf-ld)`. |
| The C library | `make lib` (writes `build/libmmdbio.a`) |

`.github/workflows/ci.yml` builds and tests all of these, plus macOS,
FreeBSD, OpenBSD, NetBSD, DragonFly BSD and illumos, on every change.

### Release files

`./build-release.sh` builds, tests and packs everything into `dist/`:

- `mmdbio-<version>-<os>-<arch>.tar.gz` (`.zip` for Windows), with `mmdbio`,
  the C library and its header, this README and the license;
- on Linux, a `.deb` and an `.rpm` with shell completion, when `dpkg-deb` and
  `rpmbuild` are installed;
- on 64-bit Linux, also the 32-bit, ARM and Windows builds, each tested under
  qemu or wine when those are installed (the top of the script lists the
  compilers each one needs);
- `SHA256SUMS`, to check the downloads.

`.github/workflows/release.yml` runs this on every system when a tag like
`v2.0.0` is pushed, and publishes the files on the releases page. A tag with
a hyphen, like `v2.0.0-rc.1`, is published as a pre-release, which the
install scripts don't pick. The `.deb` maintainer is taken from
`$DEB_MAINTAINER`, else from your git user.

### Source layout

```
src/            the program
  main.c, flags.*          command line options, help text, which command runs
  cmd_*.c                  one file per command
  mmdb_read.*              reading MMDB files: lookups, going through networks, verify
  shrink.*, shrink_check.c shrink, and its checks
  tabular.*                CSV, TSV and JSON Lines output for read and export
  libmmdbio.c              the C library (include/mmdbio.h)
  compat.*, color.*        differences between systems (mostly Windows); colours
  jsonw.*                  JSON output
  netaddr.*                reading and writing IP addresses, networks and ranges
  input.*                  reading files, standard input and gzip (zlib is included)
  csv.*, jstream.*         reading CSV and JSON
  rows.*                   turning input rows into networks and records
  tables.*                 lookup files and template tables
  tmpl.*                   record templates
  shared.*                 large template parts, stored once during a build
  bconfig.c, build.h       import settings and the config file
  engine.c                 the build: sorting rows, handling overlaps
  streamwriter.*           writing the database to disk
  mmdb_enc.*               writing records into the file
tests/
  unit_tests.c             small tests of the building blocks
  run_tests.sh             runs every command, then the checks below
  check_build.py           compares builds with a simple model of every address
  check_robust.py          broken and strange files fail cleanly
  check_shrink.py          shrink on hand-made files
  check_features.py        options, formats, diff, import settings, completion
  check_edges.py           builds with every feature, read back and checked
  check_template.py        record templates, order and vars
  lib_test.c               the C library
  mmdb_py.py               a separate MMDB reader and writer used by the checks
packaging/                 .deb and .rpm builders, the Homebrew formula
install.sh, install.ps1    the install scripts
third_party/zlib/          zlib 1.3.1 (only the part that reads gzip), with its license
```

## Credits and license

mmdbio is under the Apache License 2.0; see `LICENSE`. The included zlib is
under the zlib license (`third_party/zlib/LICENSE`).

The `shrink` command follows the method of IPinfo's
[`mmdbshrink`](https://github.com/ipinfo/mmdbctl/tree/master/mmdbshrink).
