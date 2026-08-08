# mmdbio: MMDB File Management CLI

mmdbio is a command line tool for reading, exporting, importing, comparing, inspecting, verifying, and analyzing MMDB files. It is built for developers who work with IP geolocation databases, threat intelligence feeds, or any dataset stored in the MMDB binary format, and need a fast way to query, convert, validate, and audit that data without writing custom code.

![License](https://img.shields.io/badge/license-Apache--2.0-blue)
![Go](https://img.shields.io/badge/built%20with-Go-00ADD8)

---

## Table of Contents

- [What Is an MMDB File](#what-is-an-mmdb-file)
- [Why Use mmdbio](#why-use-mmdbio)
- [Installation](#installation)
  - [Install with Go](#install-with-go)
  - [Build from Source](#build-from-source)
  - [Download Prebuilt Binaries](#download-prebuilt-binaries)
- [Commands](#commands)
  - [read](#read)
  - [metadata](#metadata)
  - [export](#export)
  - [import](#import)
  - [diff](#diff)
  - [inspect](#inspect)
  - [stats](#stats)
  - [verify](#verify)
  - [completion](#completion)
- [Use Cases](#use-cases)
- [Frequently Asked Questions](#frequently-asked-questions)
- [License](#license)

---

## What Is an MMDB File

MMDB is a binary database format built for fast, offline IP address lookups. It maps IP ranges to structured data such as country, city, region, ASN, or any custom fields you define, and it is widely used across the IP geolocation and network intelligence space because lookups stay fast even at millions of records.

mmdbio gives you a single tool to read, edit, validate, and compare MMDB files directly from the command line, so you do not need to write a custom script every time you need to inspect or convert one.

---

## Why Use mmdbio

- Look up IP addresses instantly from any MMDB file, one IP at a time, in bulk from a file, or across an entire CIDR range.
- Convert JSON datasets into production ready MMDB files with `import`.
- Export any MMDB file back to JSON for auditing, backups, or migration.
- Track schema and data changes between two versions of a database with `diff`.
- Validate an MMDB file before deploying it to production with `verify`.
- Inspect unfamiliar MMDB files and generate a schema from them with `inspect`.
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
git clone github.com/IPGeolocation/mmdbio
cd mmdbio
go build -o mmdbio .
```

You can now use `./mmdbio` to run the CLI.

### Download Prebuilt Binaries

Prebuilt binaries are available on the GitHub Releases page, so you do not need Go installed to use the tool. Always check the Releases page for the latest version number before downloading.

| Platform | Architecture | File Name                       |
|----------|--------------|---------------------------------|
| Linux    | amd64        | mmdbio-1.1.0-linux-amd64.tar.gz |
| Linux    | arm64        | mmdbio-1.1.0-linux-arm64.tar.gz |
| macOS    | amd64        | mmdbio-1.1.0-darwin-amd64.tar.gz|
| macOS    | arm64        | mmdbio-1.1.0-darwin-arm64.tar.gz|
| Windows  | amd64        | mmdbio-1.1.0-windows-amd64.zip  |

#### Linux

1. Download the `.tar.gz` file for your architecture.
2. Extract it to a folder in your PATH, for example `/usr/local/bin`:

   ```bash
   tar -xzf mmdbio-1.1.0-linux-amd64.tar.gz -C /usr/local/bin
   ```

3. Rename the binary for simplicity:

   ```bash
   mv /usr/local/bin/mmdbio-1.1.0-linux-amd64 /usr/local/bin/mmdbio
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
   tar -xzf mmdbio-1.1.0-darwin-amd64.tar.gz -C /usr/local/bin
   ```

3. Rename the binary:

   ```bash
   mv /usr/local/bin/mmdbio-1.1.0-darwin-amd64 /usr/local/bin/mmdbio
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
2. Extract `mmdbio-1.1.0-windows-amd64.exe` to a folder included in your system PATH.
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

### read

Read IP data from an MMDB file. Supports a single IP, batch input from a file, or a full CIDR range.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--ip`: a single IP address to look up, for example `8.8.8.8`. This is different from the `--ip` flag under `import`, which sets an IP version instead of an address.
- `--fields`: comma separated list of fields to extract, for example `location.country.name,location.city.name`.
- `--input`: path to a file containing a list of IPs, or `-` to read from stdin.
- `--out`: optional path to write results to instead of printing them.
- `--range`: a CIDR range to look up every IP within it.

**Usage examples:**

```bash
# Single IP lookup
mmdbio read --db ip-to-city.mmdb --ip 8.8.8.8

# Single IP lookup with specific fields
mmdbio read --db ip-to-city.mmdb --ip 8.8.8.8 --fields location.country.name.en,location.city.name.en

# Batch IP lookup from a file
mmdbio read --db ip-to-city.mmdb --input ips.txt --out results.json

# CIDR range lookup
mmdbio read --db ip-to-city.mmdb --range 192.168.1.0/30
```

### metadata

Show metadata information stored inside an MMDB file, such as build time, IP version, and record size.

**Flags:**

- `--db` (required): path to the `.mmdb` file.

**Usage:**

```bash
mmdbio metadata --db ip-to-city.mmdb
```

### export

Export all records from an MMDB file to JSON. Supports optional field and CIDR range filtering.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--out` (required): path to the output JSON file.
- `--fields`: comma separated list of fields to extract.
- `--range`: comma separated CIDR ranges to filter which networks get exported.

**Usage:**

```bash
# Export the entire database
mmdbio export --db ip-to-city.mmdb --out output.json

# Export with field filtering
mmdbio export --db ip-to-city.mmdb --fields location.country.name,location.city.name --out output.json

# Export only certain ranges
mmdbio export --db ip-to-city.mmdb --range 192.168.0.0/24,10.0.0.0/8 --out output.json
```

### import

Import JSON data into a new MMDB file. This is the reverse of `export`, and is useful for building custom databases or converting data from another source into MMDB format.

**Sample JSON:**

```json
{
  "8.8.8.0/24": {
    "country": "United States",
    "city": "Mountain View",
    "continent": "North America",
    "latitude": 37.4056,
    "longitude": -122.0775
  },
  "1.1.1.0/24": {
    "country": "Australia",
    "city": "Sydney",
    "continent": "Oceania",
    "latitude": -33.8688,
    "longitude": 151.2093
  }
}
```

**Flags:**

- `--in, -i` (required): input JSON file path, typically produced by the `export` command.
- `--out, -o` (required): output `.mmdb` file path.
- `--ip`: IP version to build, `4` or `6`. Default is `6`. This is different from the `--ip` flag under `read`, which takes a literal IP address instead of a version number.
- `--size`: record size for the MMDB file, `24`, `28`, or `32`. Default is `32`.
- `--merge`: merge strategy for duplicate entries. `none` keeps the first entry and ignores later duplicates, `toplevel` overwrites only the top level fields of a duplicate entry, and `recurse` merges nested fields recursively instead of overwriting them wholesale. Default is `none`.
- `--alias-6to4`: enable IPv6 to IPv4 aliasing, useful when you are building a hybrid database that needs to answer lookups for both IP versions.
- `--disallow-reserved`: skip reserved IP ranges, for example `127.0.0.0/8`.
- `--title, -t`: title for the `.mmdb` database. Default is `Custom-ip-database`.
- `--description, -d`: description for the `.mmdb` database. Default is `Custom IP Intelligence Database`.

**Usage:**

```bash
# Import IPv4 data
mmdbio import \
  --in ipv4_export.json \
  --out ipv4_data.mmdb \
  --ip 4 \
  --size 32

# Import IPv6 data
mmdbio import \
  --in ipv6_export.json \
  --out ipv6_data.mmdb \
  --ip 6

# Import mixed IPv4 and IPv6 data, which requires aliasing
mmdbio import \
  --in all_data.json \
  --out all.mmdb \
  --ip 6 \
  --alias-6to4

# Import while skipping reserved IPs
mmdbio import \
  --in dataset.json \
  --out filtered.mmdb \
  --disallow-reserved

# Import with a custom title and description
mmdbio import \
  --in data.json \
  --out custom.mmdb \
  --title "Threat DB" \
  --description "Custom threat intelligence database"
```

**Notes:**

- Input JSON keys must be CIDR blocks, single IPs, or IP ranges, with the record data as values.
- Nested maps and arrays are automatically converted into MMDB types.
- Duplicate ranges are resolved according to the `--merge` strategy.
- Warnings for invalid entries are printed to `stderr` instead of stopping the import.

### diff

Compare two MMDB files and list every network that was added, removed, or modified between them. Useful for auditing changes before you promote a new database build to production.

**Flags:**

- `--old` (required): path to the old MMDB file.
- `--new` (required): path to the new MMDB file.
- `--summary`: show only summary counts instead of a full list.
- `--json`: output results as JSON.

**Usage:**

```bash
# Compare two databases
mmdbio diff --old old.mmdb --new new.mmdb

# Summary only
mmdbio diff --old old.mmdb --new new.mmdb --summary

# JSON output for scripting or CI pipelines
mmdbio diff --old old.mmdb --new new.mmdb --json
```

### inspect

Inspect the structure of an MMDB file and optionally export that structure as a JSON schema. Useful for exploring a database you did not build yourself.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--sample-ip`: sample IP address used to walk the database structure. Defaults to `4.7.229.0` if not provided.
- `--out`: optional path to export the schema as JSON.

**Usage:**

```bash
# Inspect database structure
mmdbio inspect --db ip-to-city.mmdb

# Inspect and export the schema
mmdbio inspect --db ip-to-city.mmdb --out schema.json
```

### stats

Display statistics about an MMDB file, such as record counts and coverage.

**Flags:**

- `--db` (required): path to the `.mmdb` file.
- `--json`: output in JSON format.

**Usage:**

```bash
# View stats
mmdbio stats --db ip-to-city.mmdb

# View stats in JSON
mmdbio stats --db ip-to-city.mmdb --json
```

### verify

Verify that an MMDB file is well formed and readable. This is a good check to run in CI before shipping a newly built database.

**Flags:**

- `--db` (required): path to the `.mmdb` file.

**Usage:**

```bash
mmdbio verify --db ip-to-city.mmdb
```

**Behavior:**

- Prints `valid` if the MMDB file is valid.
- Prints `invalid` plus an error message if it is not.
- Exits with code `0` if valid, and `1` if invalid, so it can be used directly in build scripts.

### completion

Generate shell completion scripts for `mmdbio`, for use with Bash, Zsh, Fish, and PowerShell.

**Usage:**

```bash
mmdbio completion [bash|zsh|fish|powershell]
```

**Available shells:**

- `bash`
- `zsh`
- `fish`
- `powershell`

**Bash**

Load completions for the current session:

```bash
source <(mmdbio completion bash)
```

Install completions permanently:

```bash
# Linux
mmdbio completion bash > /etc/bash_completion.d/mmdbio

# macOS
mmdbio completion bash > /usr/local/etc/bash_completion.d/mmdbio
```

**Zsh**

Load completions:

```bash
echo "autoload -U compinit; compinit" >> ~/.zshrc
mmdbio completion zsh > "${fpath[1]}/_mmdbio"
```

**Fish**

Load completions:

```bash
mmdbio completion fish | source
```

Install completions permanently:

```bash
mmdbio completion fish > ~/.config/fish/completions/mmdbio.fish
```

**PowerShell**

Load completions:

```powershell
mmdbio completion powershell | Out-String | Invoke-Expression
```

Install completions permanently:

```powershell
mmdbio completion powershell > mmdbio.ps1
```

**Notes:**

- Use the file path that matches your shell when installing completions permanently.
- The `completion` command accepts exactly one argument, which must be one of the shells listed above.

---

## Use Cases

- **IP geolocation lookups in scripts and pipelines.** Use `read` to pull country, city, or custom fields for a single IP, a list of IPs, or a full CIDR block.
- **Building a custom MMDB file from your own data.** Use `import` to turn a JSON dataset, such as a threat list or a custom geolocation feed, into a fast MMDB file.
- **Auditing database releases before deployment.** Use `diff` to see exactly what changed between two builds of a database, and `verify` to confirm the new build is not corrupted before you ship it.
- **Exploring an unfamiliar MMDB file.** Use `inspect` and `metadata` to understand the schema and structure of a database file you did not build.
- **Migrating data between formats.** Use `export` to pull a database out to JSON, edit or transform it, then `import` it back into a new MMDB file.

---

## Frequently Asked Questions

<details>
<summary><strong>What is an MMDB file?</strong></summary>
MMDB is a binary file format built for fast, offline IP address lookups. It stores IP ranges mapped to structured data such as location or network information and is read directly into memory for sub-millisecond queries.
</details>

<details>
<summary><strong>Can I look up a single IP address from the command line?</strong></summary>
Yes. Run <code>mmdbio read --db yourfile.mmdb --ip 8.8.8.8</code> to look up a single IP address. Add <code>--range</code> to look up every IP in a CIDR block at once.
</details>

<details>
<summary><strong>How do I convert JSON data into an MMDB file?</strong></summary>
Use the <code>import</code> command with <code>--in</code> pointing to your JSON file and <code>--out</code> pointing to the MMDB file you want to create. See the <code>import</code> section for a full example.
</details>

<details>
<summary><strong>How do I compare two versions of a database?</strong></summary>
Use <code>mmdbio diff --old old.mmdb --new new.mmdb</code> to see every network that was added, removed, or changed between the two files. Add <code>--summary</code> for a quick count or <code>--json</code> for scripting.
</details>

<details>
<summary><strong>How do I check if an MMDB file is valid before deploying it?</strong></summary>
Run <code>mmdbio verify --db yourfile.mmdb</code>. It prints <code>valid</code> or <code>invalid</code> and exits with code <code>0</code> or <code>1</code>, making it suitable for use in a CI pipeline.
</details>

---

## License

Released under the Apache-2.0 license. See the [LICENSE](LICENSE) file for details.
