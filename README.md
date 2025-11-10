# MMDB CLI Tool Documentation

This documentation provides an overview and usage guide for the MMDB CLI tool. The tool allows working with MaxMind MMDB files including reading, exporting, comparing, inspecting, verifying, and viewing statistics.

---

## Table of Contents

- [Installation](#installation)
- [Commands](#commands)
  - [read](#read)
  - [metadata](#metadata)
  - [export](#export)
  - [diff](#diff)
  - [inspect](#inspect)
  - [stats](#stats)
  - [verify](#verify)
- [Examples](#examples)

---

## Installation

Ensure you have Go installed and set up. Clone the repository and build the CLI:

```bash
git clone github.com/IPGeolocation/mmdbio
cd mmdbio
go build -o mmdbio .
```

You can now use `./mmdbio` to run the CLI.

---

## Commands

### read

**Description:** Read IP data from an MMDB file. Supports single IP, batch input, or CIDR range.

**Flags:**

- `--db` (required): Path to the `.mmdb` file.
- `--ip`: Single IP to lookup.
- `--fields`: Comma-separated list of fields to extract (e.g., `location.country.name,city.names.en`).
- `--input`: Path to file with IPs (or `-` for stdin).
- `--out`: Optional output file path.
- `--range`: CIDR range to lookup all IPs.

**Usage Examples:**

```bash
# Single IP lookup
mmdbio read --db GeoIP2-CitFlags:

--db (required): Path to the .mmdb file.
--sample-ip: Sample IP to inspect structure (defaults to 4.7.229.0 if not provided).
--out: Optional path to export schema as JSON.y.mmdb --ip 8.8.8.8

# Batch IP lookup from file
mmdbio read --db GeoIP2-City.mmdb --input ips.txt --out results.json

# CIDR range lookup
mmdbio read --db GeoIP2-City.mmdb --range 192.168.1.0/30
```

---

### metadata

**Description:** Show metadata information from an MMDB file.

**Flags:**

- `--db` (required): Path to the `.mmdb` file.

**Usage:**

```bash
mmdbio metadata --db GeoIP2-City.mmdb
```

---

### export

**Description:** Export all records from an MMDB to JSON. Supports optional field and CIDR range filtering.

**Flags:**

- `--db` (required): Path to the `.mmdb` file.
- `--out` (required): Path to output JSON file.
- `--fields`: Comma-separated list of fields to extract.
- `--range`: Comma-separated CIDR ranges to filter networks.

**Usage:**

```bash
# Export entire database
mmdbio export --db GeoIP2-City.mmdb --out output.json

# Export with field filtering
mmdbio export --db GeoIP2-City.mmdb --fields location.country.name,city.names.en --out output.json

# Export only certain ranges
mmdbio export --db GeoIP2-City.mmdb --range 192.168.0.0/24,10.0.0.0/8 --out output.json
```

---

### diff

**Description:** Compare two MMDB files and show differences. Lists networks that were added, removed, or modified.

**Flags:**

- `--old` (required): Path to the old MMDB file.
- `--new` (required): Path to the new MMDB file.
- `--summary`: Show only summary counts.
- `--json`: Output results as JSON.

**Usage:**

```bash
# Compare databases
mmdbio diff --old old.mmdb --new new.mmdb

# Summary only
mmdbio diff --old old.mmdb --new new.mmdb --summary

# JSON output
mmdbio diff --old old.mmdb --new new.mmdb --json
```

---

### inspect

**Description:** Inspect MMDB structure and optionally export schema to JSON.

**Flags:**

- `--db` (required): Path to the `.mmdb` file.
- `--sample-ip`: Sample IP to inspect structure (defaults to `4.7.229.0` if not provided).
- `--out`: Optional path to export schema as JSON.

**Usage:**

```bash
# Inspect database structure
mmdbio inspect --db GeoIP2-City.mmdb

# Inspect and export schema
mmdbio inspect --db GeoIP2-City.mmdb --out schema.json
```

---

### stats

**Description:** Display statistics and metadata of an MMDB file.

**Flags:**

- `--db` (required): Path to the `.mmdb` file.
- `--json`: Output in JSON format.

**Usage:**

```bash
# View stats
mmdbio stats --db GeoIP2-City.mmdb

# View stats in JSON
mmdbio stats --db GeoIP2-City.mmdb --json
```

---

### verify

**Description:** Verify the validity of an MMDB file.

**Flags:**

- `--db` (required): Path to the `.mmdb` file.

**Usage:**

```bash
# Verify MMDB validity
mmdbio verify --db GeoIP2-City.mmdb
```

**Behavior:**

- Prints `valid` if the MMDB is valid.
- Prints `invalid` and error message if the MMDB is invalid.
- Exit code `0` if valid, `1` if invalid.

---

## Examples

```bash
# Single IP lookup
mmdbio read --db GeoIP2-City.mmdb --ip 8.8.8.8 --fields location.country.name

# Export database
mmdbio export --db GeoIP2-City.mmdb --out export.json --fields location.country.name,city.names.en

# Compare old and new databases
mmdbio diff --old old.mmdb --new new.mmdb --summary

# Inspect database structure and export schema
mmdbio inspect --db GeoIP2-City.mmdb --out schema.json

# View statistics
mmdbio stats --db GeoIP2-City.mmdb --json

# Verify MMDB
mmdbio verify --db GeoIP2-City.mmdb
```

---
