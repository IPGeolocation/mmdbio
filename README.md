# mmdbio: MMDB and GeoIP command-line toolkit

mmdbio is a fast command-line tool for reading, querying, converting, comparing, inspecting, and validating MMDB files. MMDB is the compact binary database format used for offline IP geolocation, GeoIP, and IP intelligence lookups. mmdbio is built for developers who work with IP geolocation databases, threat intelligence feeds, or any dataset stored in the MMDB format, and who need a quick way to look up IP addresses, convert data to and from JSON, validate a build, and audit changes without writing custom code.

[![Latest release](https://img.shields.io/github/v/release/IPGeolocation/mmdbio)](https://github.com/IPGeolocation/mmdbio/releases)
[![License: Apache-2.0](https://img.shields.io/github/license/IPGeolocation/mmdbio)](LICENSE)

You can use mmdbio with any MMDB file, including the IP geolocation and IP intelligence databases from [IPGeolocation.io](https://ipgeolocation.io).

---

## Table of Contents

- [Quickstart](#quickstart)
- [What Is an MMDB File](#what-is-an-mmdb-file)
- [Why Use mmdbio](#why-use-mmdbio)
- [Installation](#installation)
  - [Install with Go](#install-with-go)
  - [Build from source](#build-from-source)
  - [Download prebuilt binaries](#download-prebuilt-binaries)
- [Commands](#commands)
  - [read: look up IP data in an MMDB file](#read-look-up-ip-data-in-an-mmdb-file)
  - [metadata: show MMDB metadata](#metadata-show-mmdb-metadata)
  - [export: convert an MMDB file to JSON](#export-convert-an-mmdb-file-to-json)
  - [import: build an MMDB database from JSON](#import-build-an-mmdb-database-from-json)
  - [diff: compare two MMDB files](#diff-compare-two-mmdb-files)
  - [inspect: discover an MMDB schema](#inspect-discover-an-mmdb-schema)
  - [stats: show record counts and coverage](#stats-show-record-counts-and-coverage)
  - [verify: validate an MMDB file](#verify-validate-an-mmdb-file)
  - [completion: shell completion scripts](#completion-shell-completion-scripts)
- [Field Paths and Schemas](#field-paths-and-schemas)
- [Automation and CI](#automation-and-ci)
- [Use Cases](#use-cases)
- [Frequently Asked Questions](#frequently-asked-questions)
- [Contributing](#contributing)
- [Changelog](#changelog)
- [Support](#support)
- [License](#license)

---

## Quickstart

Install the CLI, then look up an IP address in any MMDB file:

```bash
# Install (requires Go 1.24.5 or newer)
go install github.com/IPGeolocation/mmdbio@latest

# Look up a single IP in your .mmdb file
mmdbio read --db ip-to-city.mmdb --ip 8.8.8.8
```

**Output:**

```json
{
  "location": {
    "city": {
      "name": "Mountain View"
    },
    "continent": {
      "name": "North America"
    },
    "country": {
      "iso_code": "US",
      "name": "United States"
    },
    "latitude": 37.4056,
    "longitude": -122.0775
  }
}

```

The exact fields depend on the schema of your database. See [Field Paths and Schemas](#field-paths-and-schemas). If you do not have an MMDB file yet, see [import](#import-build-an-mmdb-database-from-json) to build one from a few lines of JSON.

---

## What Is an MMDB File

MMDB is a compact binary database format built for fast, offline IP address lookups. It maps IP ranges to structured records such as country, city, region, ASN, proxy or threat attributes, or any custom fields you define. It stays fast even at millions of records because the database is memory mapped and searched with a prefix tree, so a lookup only walks the bits of the queried IP.

The format is widely used across GeoIP, IP geolocation, and network intelligence workflows. Field names and record shapes are not fixed by the format itself. Each database defines its own schema, so a city database and a threat database can look very different. mmdbio gives you one tool to read, convert, validate, and compare these files directly from the command line, so you do not need to write a custom script every time you inspect or convert one.

---

## Why Use mmdbio

- Look up IP addresses from any MMDB file: one IP at a time, in bulk from a file, or across an entire CIDR range.
- Convert JSON datasets into production-ready MMDB files with `import`.
- Convert any MMDB file back to JSON for auditing, backups, or migration with `export`.
- Track schema and data changes between two builds of a database with `diff`.
- Validate an MMDB file before you deploy it with `verify`, which returns a non-zero exit code on failure so it works in CI.
- Discover the schema of an unfamiliar MMDB file with `inspect`.
- Runs entirely offline. No API calls or external services required.

---

## Installation

### Install with Go

To install `mmdbio` using `go install`, run:

```bash
go install github.com/IPGeolocation/mmdbio@latest
```

Make sure `$GOBIN` or `$GOPATH/bin` is in your `PATH`, then run:

```bash
mmdbio --help
```

If `go install` fails due to proxy caching, use:

```bash
GOPROXY=direct go install github.com/IPGeolocation/mmdbio@latest
```

### Build from Source

Ensure you have Go installed and set up. Clone the repository and build the CLI:

```bash
git clone github.com/IPGeolocation/mmdbio.git
cd mmdbio
go build -o mmdbio .
```

You can now use `./mmdbio` to run the CLI.

### Download Prebuilt Binaries

Prebuilt binaries are available on the [GitHub Releases page](https://github.com/IPGeolocation/mmdbio/releases), so you do not need Go installed to use the tool. Always check the Releases page for the latest version number before downloading.

| Platform | Architecture | File Name                       |
|----------|--------------|---------------------------------|
| Linux    | amd64        | mmdbio-<version>-linux-amd64.tar.gz |
| Linux    | arm64        | mmdbio-<version>-linux-arm64.tar.gz |
| macOS    | amd64        | mmdbio-<version>-darwin-amd64.tar.gz|
| macOS    | arm64        | mmdbio-<version>-darwin-arm64.tar.gz|
| Windows  | amd64        | mmdbio-<version>-windows-amd64.zip  |

#### Linux

1. Download the `.tar.gz` file for your architecture.
2. Extract it to a folder in your PATH, for example `/usr/local/bin`:

   ```bash
   tar -xzf mmdbio-<version>-linux-amd64.tar.gz -C /usr/local/bin
   ```

3. Rename the binary for simplicity:

   ```bash
   mv /usr/local/bin/mmdbio-<version>-linux-amd64 /usr/local/bin/mmdbio
   ```

4. Make the binary executable:

   ```bash
   chmod +x /usr/local/bin/mmdbio
   ```

5. Verify installation:

   ```bash
   mmdbio --help
   ```

#### macOS

1. Download the `.tar.gz` file for your architecture, amd64 or arm64.
2. Extract it to a folder in your PATH, for example `/usr/local/bin`:

   ```bash
   tar -xzf mmdbio-<version>-darwin-amd64.tar.gz -C /usr/local/bin
   ```

3. Rename the binary:

   ```bash
   mv /usr/local/bin/mmdbio-<version>-darwin-amd64 /usr/local/bin/mmdbio
   ```

4. Make it executable:

   ```bash
   chmod +x /usr/local/bin/mmdbio
   ```

5. Verify installation:

   ```bash
   mmdbio --help
   ```

#### Windows

1. Download the `.zip` file.
2. Extract `mmdbio-<version>-windows-amd64.exe` to a folder included in your system PATH.
3. Rename the file to `mmdbio.exe` for convenience.
4. Open Command Prompt and verify:

   ```cmd
   mmdbio --help
   ```

#### Notes

- Confirm execution permissions on Linux and macOS.
- A good default folder for binaries is `/usr/local/bin`, or any folder already in your PATH.
- For updates, check the GitHub Releases page.

#### Troubleshooting

- **Command not found:** confirm the binary sits in a folder included in your PATH.
- **Execution permission error:** run `chmod +x <binary>` on Linux or macOS.
- **Wrong architecture:** download the binary that matches your OS and CPU architecture.

---

## Commands

Every example below uses a small sample city database built from the JSON shown in the [import](#import-build-an-mmdb-database-from-json) section, so the commands and their output are consistent throughout. Values such as node counts and build dates reflect that sample database and will differ for your own files.

### read: look up IP data in an MMDB file

Look up IP data from an MMDB file. Supports a single IP, batch input from a file or stdin, or a full CIDR range.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--ip`: a single IP address to look up, for example `8.8.8.8`. In this command `--ip` takes a literal address. In `import` the same flag name selects an IP version instead. See [import](#import-build-an-mmdb-database-from-json).
- `--fields`: comma-separated list of dotted field paths to extract, for example `location.country.name,location.city.name`.
- `--input`: path to a file containing one IP per line, or `-` to read from stdin.
- `--out`: optional path to write results to instead of printing them.
- `--range`: a single CIDR range. Every address in the range is looked up. In `export` the `--range` flag accepts a comma-separated list instead.

**Single IP lookup:**

```bash
mmdbio read --db ip-to-city.mmdb --ip 8.8.8.8
```

```json
{
  "location": {
    "city": {
      "name": "Mountain View"
    },
    "continent": {
      "name": "North America"
    },
    "country": {
      "iso_code": "US",
      "name": "United States"
    },
    "latitude": 37.4056,
    "longitude": -122.0775
  }
}
```

If the IP is not present in the database, `read` prints `No data found for that IP.` and exits with code `0`.

**Single IP with selected fields:**

```bash
mmdbio read --db ip-to-city.mmdb --ip 8.8.8.8 --fields location.country.name,location.city.name
```

```json
{
  "8.8.8.8": {
    "location.city.name": "Mountain View",
    "location.country.name": "United States"
  }
}
```

**Batch lookup from a file:**

With `ips.txt` containing `8.8.8.8` and `1.1.1.1`, one per line:

```bash
mmdbio read --db ip-to-city.mmdb --input ips.txt --out results.json
```

`results.json` contains an object keyed by IP:

```json
{
  "1.1.1.1": {
    "location": {
      "city": { "name": "Sydney" },
      "continent": { "name": "Oceania" },
      "country": { "iso_code": "AU", "name": "Australia" },
      "latitude": -33.8688,
      "longitude": 151.2093
    }
  },
  "8.8.8.8": {
    "location": {
      "city": { "name": "Mountain View" },
      "continent": { "name": "North America" },
      "country": { "iso_code": "US", "name": "United States" },
      "latitude": 37.4056,
      "longitude": -122.0775
    }
  }
}
```

**CIDR range lookup:**

```bash
mmdbio read --db ip-to-city.mmdb --range 8.8.8.0/30
```

`--range` enumerates every address in the range and looks each one up individually, then returns an object keyed by IP. A `/30` returns four addresses. Be careful with large prefixes: a `/16` is 65,536 lookups and a `/8` is more than 16 million, which will use a large amount of memory and time. Use a small, routable prefix. Private ranges such as `192.168.0.0/16` return `null` for each address in a public geolocation database, because those addresses are not present in the data.

### metadata: show MMDB metadata

Show the metadata stored inside an MMDB file, such as database type, IP version, record size, node count, build date, and languages.

**Flags:**

- `--db` (required): path to the `.mmdb` file.

```bash
mmdbio metadata --db ip-to-city.mmdb
```

```text
MMDB Metadata
-----------------------------
Database Type: Demo-City-DB
IP Version:    4
Record Size:   32 bits
Node Count:    59
Build Date:    Tue, 11 Aug 2026 05:44:33 UTC
Languages:     [en]
Description: {
    "en": "Demo IP to City database"
  }
```

### export: convert an MMDB file to JSON

Export every record from an MMDB file to JSON. Supports optional field selection and CIDR range filtering. This is the reverse of `import` and is useful for auditing, backups, migration, or converting an MMDB file to JSON for another tool.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--out` (required): path to the output JSON file.
- `--fields`: comma-separated list of dotted field paths to extract.
- `--range`: optional comma-separated list of CIDR ranges. Only networks that fall within these ranges are exported. This differs from `read --range`, which accepts a single CIDR.

**Export the entire database:**

```bash
mmdbio export --db ip-to-city.mmdb --out output.json
```

```bash
Exported 3 records to output.json
```

The output file maps each network to its record:

```json
{
  "1.1.1.0/24": {
    "location": {
      "city": { "name": "Sydney" },
      "continent": { "name": "Oceania" },
      "country": { "iso_code": "AU", "name": "Australia" },
      "latitude": -33.8688,
      "longitude": 151.2093
    }
  },
  "8.8.8.0/24": {
    "location": {
      "city": { "name": "Mountain View" },
      "continent": { "name": "North America" },
      "country": { "iso_code": "US", "name": "United States" },
      "latitude": 37.4056,
      "longitude": -122.0775
    }
  },
  "9.9.9.0/24": {
    "location": {
      "city": { "name": "Zurich" },
      "continent": { "name": "Europe" },
      "country": { "iso_code": "CH", "name": "Switzerland" },
      "latitude": 47.3769,
      "longitude": 8.5417
    }
  }
}
```

**Export selected fields:**

```bash
mmdbio export --db ip-to-city.mmdb --fields location.country.name,location.city.name --out output.json
```

```json
{
  "1.1.1.0/24": {
    "location.city.name": "Sydney",
    "location.country.name": "Australia"
  },
  "8.8.8.0/24": {
    "location.city.name": "Mountain View",
    "location.country.name": "United States"
  },
  "9.9.9.0/24": {
    "location.city.name": "Zurich",
    "location.country.name": "Switzerland"
  }
}
```

**Export only certain ranges:**

```bash
mmdbio export --db ip-to-city.mmdb --range 8.8.8.0/24,9.9.9.0/24 --out output.json
```

### import: build an MMDB database from JSON

Build a new MMDB file from JSON. This is the reverse of `export`, and is the way to turn a threat list, a custom geolocation feed, or any exported data back into a fast MMDB database.

You define the schema. Whatever nested structure you put in the JSON values becomes the record structure in the database, and therefore the dotted field paths you later pass to `read --fields` and `export --fields`. The examples throughout this document are built from this file, saved as `city_data.json`:

```json
{
  "8.8.8.0/24": {
    "location": {
      "country": { "name": "United States", "iso_code": "US" },
      "city": { "name": "Mountain View" },
      "continent": { "name": "North America" },
      "latitude": 37.4056,
      "longitude": -122.0775
    }
  },
  "1.1.1.0/24": {
    "location": {
      "country": { "name": "Australia", "iso_code": "AU" },
      "city": { "name": "Sydney" },
      "continent": { "name": "Oceania" },
      "latitude": -33.8688,
      "longitude": 151.2093
    }
  },
  "9.9.9.0/24": {
    "location": {
      "country": { "name": "Switzerland", "iso_code": "CH" },
      "city": { "name": "Zurich" },
      "continent": { "name": "Europe" },
      "latitude": 47.3769,
      "longitude": 8.5417
    }
  }
}
```

Each key must be a CIDR block (`1.2.3.0/24`), a single IP (`8.8.8.8`), or an IP range (`1.2.3.0-1.2.3.255`).

**Flags:**

- `--in, -i` (required): input JSON file path, typically produced by `export`.
- `--out, -o` (required): output `.mmdb` file path.
- `--ip`: IP version to build, `4` or `6`. Default is `6`. An IPv6 tree can also hold IPv4 data through aliasing, so `6` is the safe default when a dataset might contain either. Use `4` for an IPv4-only database. Note that in `read` the `--ip` flag means a literal IP address, not a version.
- `--size`: record size in bits, `24`, `28`, or `32`. Default is `32`.
- `--merge`: strategy for duplicate entries. `none` keeps the first entry and ignores later duplicates. `toplevel` overwrites only the top-level fields of a duplicate. `recurse` merges nested fields recursively instead of overwriting them wholesale. Default is `none`.
- `--alias-6to4`: enable IPv6 to IPv4 aliasing. Use this when you build a hybrid IPv6 database (`--ip 6`) that must also answer IPv4 lookups. It is the companion to the default `--ip 6` behavior described above.
- `--disallow-reserved`: skip reserved IP ranges such as `127.0.0.0/8`.
- `--title, -t`: title stored in the database metadata. Default is `Custom-ip-database`.
- `--description, -d`: description stored in the database metadata. Default is `Custom IP Intelligence Database`.

**Build an IPv4 database:**

```bash
mmdbio import --in city_data.json --out ip-to-city.mmdb --ip 4 --size 32
```

```bash
✅ Successfully wrote 3 entries to ip-to-city.mmdb
```

**Build an IPv6 database:**

```bash
mmdbio import --in ipv6_export.json --out ipv6_data.mmdb --ip 6
```

**Build a hybrid database that answers both IPv4 and IPv6:**

```bash
mmdbio import --in all_data.json --out all.mmdb --ip 6 --alias-6to4
```

**Build while skipping reserved IPs, with a custom title and description:**

```bash
mmdbio import \
  --in dataset.json \
  --out filtered.mmdb \
  --disallow-reserved \
  --title "Threat DB" \
  --description "Custom threat intelligence database"
```

**Notes:**

- Input JSON keys must be CIDR blocks, single IPs, or IP ranges, with the record data as values.
- Nested maps and arrays are converted into MMDB types automatically.
- Duplicate ranges are resolved according to the `--merge` strategy.
- Warnings for invalid entries are printed to `stderr` and do not stop the import.

### diff: compare two MMDB files

Compare two MMDB files and list every network that was added, removed, or modified between them. Useful for auditing changes before you promote a new database build.

**Flags:**

- `--old` (required): path to the old MMDB file.
- `--new` (required): path to the new MMDB file.
- `--summary`: show only summary counts.
- `--json`: output the result as JSON.

**Full diff:**

```bash
mmdbio diff --old old.mmdb --new new.mmdb
```

```text
Added: 1
Removed: 1
Modified: 1

Added:
   80.80.80.0/24
Removed:
   1.1.1.0/24
Modified:
   9.9.9.0/24
```

**Summary only:**

```bash
mmdbio diff --old old.mmdb --new new.mmdb --summary
```

```text
Added: 1 | Removed: 1 | Modified: 1
```

**JSON output for scripting or CI:**

```bash
mmdbio diff --old old.mmdb --new new.mmdb --json
```

The JSON groups changes under `added`, `removed`, and `modified`, and includes the full old and new record for each modified network. Record bodies are abbreviated here for space:

```json
{
  "added": {
    "80.80.80.0/24": { "location": { "city": { "name": "Berlin" }, "country": { "iso_code": "DE", "name": "Germany" } } }
  },
  "modified": {
    "9.9.9.0/24": {
      "old": { "location": { "city": { "name": "Zurich" } } },
      "new": { "location": { "city": { "name": "Geneva" } } }
    }
  },
  "removed": {
    "1.1.1.0/24": { "location": { "city": { "name": "Sydney" }, "country": { "iso_code": "AU", "name": "Australia" } } }
  }
}
```

**Note for CI:**`diff` reports differences but always exits `0` when both files are readable, even when there are changes. To fail a pipeline on changes, inspect the counts or the JSON output. See [Automation and CI](#automation-and-ci).

### inspect: discover an MMDB schema

Inspect the structure of an MMDB file and optionally export that structure as a JSON schema. Useful for exploring a database you did not build yourself and for learning the field paths to pass to `read --fields`.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--sample-ip`: an IP address that walks the record structure. It must be an address that exists in the database. If it is omitted, a built-in default address is used, and if that address is not in your database, `inspect` prints `No record found for this IP.` and shows nothing. Pass a `--sample-ip` you know is present.
- `--out`: optional path to export the discovered schema as JSON.

**Inspect the structure:**

```bash
mmdbio inspect --db ip-to-city.mmdb --sample-ip 8.8.8.8
```

```text
📂 Structure for MMDB: ip-to-city.mmdb
────────────────────────────────────────────
location.latitude                                  : float64
location.longitude                                 : float64
location.city.name                                 : string
location.continent.name                            : string
location.country.iso_code                          : string
location.country.name                              : string
```

**Export the schema to JSON:**

```bash
mmdbio inspect --db ip-to-city.mmdb --sample-ip 8.8.8.8 --out schema.json
```

`schema.json` maps each field path to its type:

```json
{
  "location.city.name": "string",
  "location.continent.name": "string",
  "location.country.iso_code": "string",
  "location.country.name": "string",
  "location.latitude": "float64",
  "location.longitude": "float64"
}
```

### stats: show record counts and coverage

Display statistics about an MMDB file, including record counts by IP version and total networks.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--json`: output in JSON format.

**Text output:**

```bash
mmdbio stats --db ip-to-city.mmdb
```

```text
=====MMDB Statistics========
────────────────────────────────────────────
Database Type: Demo-City-DB
Description:   Demo IP to City database
IP Version:    4
Record Size:   32 bits
Node Count:    59
Build Epoch:   2026-08-11T05:44:33Z
Languages:     [en]
────────────────────────────────────────────
IPv4 Count:    3
IPv6 Count:    0
Networks:      3
────────────────────────────────────────────
```

**JSON output:**

```bash
mmdbio stats --db ip-to-city.mmdb --json
```

```json
{
  "build_epoch": "2026-08-11T05:44:33Z",
  "database_type": "Demo-City-DB",
  "description": {
    "en": "Demo IP to City database"
  },
  "ip_version": 4,
  "ipv4_count": 3,
  "ipv6_count": 0,
  "languages": [
    "en"
  ],
  "networks": 3,
  "node_count": 59,
  "record_size": 32
}
```

### verify: validate an MMDB file

Verify that an MMDB file is well-formed and readable. This is the check to run in CI before you ship a newly built database, because it is the one command that signals success or failure through its exit code.

**Flags:**

- `--db` (required): path to the `.mmdb` file.

```bash
mmdbio verify --db ip-to-city.mmdb
```

```text
valid
```

**Behavior:**

- Prints `valid` and exits with code `0` when the file is valid.
- Prints `invalid:` followed by the reason and exits with code `1` when the file is missing, unreadable, or malformed.

### completion: shell completion scripts

Generate shell completion scripts for `mmdbio`, for Bash, Zsh, Fish, and PowerShell.

```bash
mmdbio completion [bash|zsh|fish|powershell]
```

**Bash**

Load completions for the current session:

```bash
source <(mmdbio completion bash)
```

Install permanently (writing to a system directory needs elevated permissions):

```bash
# Linux
mmdbio completion bash | sudo tee /etc/bash_completion.d/mmdbio > /dev/null

# macOS
mmdbio completion bash | sudo tee /usr/local/etc/bash_completion.d/mmdbio > /dev/null
```

**Zsh**

```bash
echo "autoload -U compinit; compinit" >> ~/.zshrc
mmdbio completion zsh > "${fpath[1]}/_mmdbio"
```

**Fish**

```bash
mmdbio completion fish | source
mmdbio completion fish > ~/.config/fish/completions/mmdbio.fish
```

**PowerShell**

```powershell
mmdbio completion powershell | Out-String | Invoke-Expression
mmdbio completion powershell > mmdbio.ps1
```

**Notes:**

- Use the file path that matches your shell when installing permanently.
- The `completion` command accepts exactly one argument, which must be one of the shells above.

---

## Field Paths and Schemas

The MMDB format does not fix a schema. Each database defines its own record structure, so the dotted paths you pass to `--fields` and the shape of the output depend on the specific file you are querying. A city database might expose `location.country.name` and `location.city.name`, while a threat database might expose `is_proxy`, `is_vpn`, and `threat_score`. If you copy a field path from one database into a query against another, you will get an empty result.

When you work with an unfamiliar file, discover its fields first:

```bash
mmdbio inspect --db your-database.mmdb --sample-ip <an-ip-in-the-db>
```

When you build a database with `import`, you choose the schema yourself through the structure of your JSON values, as shown in the [import](#import-build-an-mmdb-database-from-json) section.

---

## Automation and CI

Two commands are built for pipelines: `verify` gates on validity through its exit code, and `diff` reports what changed between builds.

The workflow below validates a freshly built database on every push and fails the job if the file is invalid. It reads the required Go version from `go.mod`.

```yaml
name: validate-mmdb

on:
  push:
  pull_request:

jobs:
  verify:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v5
      - uses: actions/setup-go@v6
        with:
          go-version-file: go.mod
      - name: Install mmdbio
        run: go install github.com/IPGeolocation/mmdbio@latest
      - name: Verify the database
        run: mmdbio verify --db ip-to-city.mmdb
```

Because `verify` exits non-zero on failure, the job fails automatically when the database is invalid.

`diff` always exits `0`, so to fail a job when a database changes unexpectedly you parse its output. For example, using `jq` to fail when any network changed:

```bash
mmdbio diff --old old.mmdb --new new.mmdb --json > diff.json
changed=$(jq '((.added // {}) | length) + ((.removed // {}) | length) + ((.modified // {}) | length)' diff.json)
if [ "$changed" -ne 0 ]; then
  echo "Database changed: $changed networks differ"
  exit 1
fi
```

---

## Use Cases

- **IP geolocation lookups in scripts and pipelines.** Use `read` to pull country, city, or custom fields for a single IP, a list of IPs, or a CIDR block.
- **Building a custom GeoIP or IP intelligence database.** Use `import` to turn a JSON dataset, such as a threat list or a custom geolocation feed, into a fast MMDB file.
- **Auditing database releases before deployment.** Use `diff` to see exactly what changed between two builds, and `verify` to confirm the new build is not corrupted before you ship it.
- **Exploring an unfamiliar MMDB file.** Use `inspect` and `metadata` to understand the schema and structure of a file you did not build.
- **Converting between MMDB and JSON.** Use `export` to pull a database out to JSON, edit or transform it, then `import` it back into a new MMDB file.

---

## Frequently Asked Questions

<details>
<summary><strong>What is an MMDB file?</strong></summary>
MMDB is a compact binary file format built for fast, offline IP address lookups. It maps IP ranges to structured data such as location, network, proxy, or threat attributes, and is memory mapped for fast in-memory lookups. The schema is defined by each database, not by the format itself.
</details>

<details>
<summary><strong>How do I look up a single IP address from the command line?</strong></summary>
Run <code>mmdbio read --db yourfile.mmdb --ip 8.8.8.8</code> to look up a single IP. Add <code>--range</code> to look up every address in a CIDR block, or <code>--input</code> to look up a list of IPs from a file.
</details>

<details>
<summary><strong>How do I convert an MMDB file to JSON?</strong></summary>
Use <code>mmdbio export --db yourfile.mmdb --out output.json</code>. Add <code>--fields</code> to export only selected field paths, or <code>--range</code> to export only certain networks.
</details>

<details>
<summary><strong>How do I build an MMDB database from JSON?</strong></summary>
Use the <code>import</code> command with <code>--in</code> pointing to your JSON file and <code>--out</code> pointing to the MMDB file you want to create. The structure of your JSON values becomes the schema of the database. See the <a href="#import-build-an-mmdb-database-from-json">import</a> section for a full example.
</details>

<details>
<summary><strong>How do I find out which fields a database contains?</strong></summary>
Run <code>mmdbio inspect --db yourfile.mmdb --sample-ip &lt;an-ip-in-the-db&gt;</code> to print the field paths and their types, or add <code>--out schema.json</code> to save the schema. Field paths vary from one database to another.
</details>

<details>
<summary><strong>How do I check that an MMDB file is valid before deploying it?</strong></summary>
Run <code>mmdbio verify --db yourfile.mmdb</code>. It prints <code>valid</code> and exits <code>0</code>, or prints <code>invalid:</code> with a reason and exits <code>1</code>, so it works directly in a CI pipeline.
</details>

<details>
<summary><strong>Does mmdbio work offline?</strong></summary>
Yes. Every command runs against local files. mmdbio makes no API calls and needs no external services.
</details>

---

## Contributing

Contributions are welcome. To get started:

1. Fork the repository and create a branch for your change.
2. Build locally with `go build ./...`.
3. Keep changes focused and include a clear description of what and why.
4. Open a pull request against the default branch.

If you plan a larger change, open an issue first to discuss the approach.

## Changelog

Release notes and version history are published on the [GitHub Releases page](https://github.com/IPGeolocation/mmdbio/releases).

## Support

- Questions and bug reports: open an issue on the [issue tracker](https://github.com/IPGeolocation/mmdbio/issues).
- API reference: [pkg.go.dev/github.com/IPGeolocation/mmdbio](https://pkg.go.dev/github.com/IPGeolocation/mmdbio).
- MMDB databases for IP geolocation and IP intelligence: [IPGeolocation.io](https://ipgeolocation.io/documentation/databases.html).

## License

mmdbio is released under the Apache License 2.0. See the [LICENSE](https://github.com/IPGeolocation/mmdbio/blob/main/LICENSE) file for the full text.
