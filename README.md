# MMDB CLI Tool Documentation

This documentation provides an overview and usage guide for the MMDB CLI tool. The tool allows working with MaxMind MMDB files including reading, exporting, comparing, inspecting, verifying, and viewing statistics.

---

## Table of Contents

- [Installation](#installation)
  - [1. Go install](#1-go-install)
  - [2. Download and build from source](#2-download-and-build-from-source)
  - [3. Download pre-built binaries](#3-download-pre-built-binaries)
- [Commands](#commands)
  - [read](#read)
  - [metadata](#metadata)
  - [export](#export)
  - [diff](#diff)
  - [inspect](#inspect)
  - [stats](#stats)
  - [verify](#verify)
  - [completion](#completion)
- [Examples](#examples)

---

## Installation

### 1. Go install

To install `mmdbio` using `go install`, run:

```bash
go install github.com/IPGeolocation/mmdbio@latest
```

Make sure `$GOBIN` or `$GOPATH/bin` is in your `PATH`, then run:

```bash
mmdbio --help
```

---


### 2. Download and Build from Source

Ensure you have Go installed and set up. Clone the repository and build the CLI:

```bash
git clone github.com/IPGeolocation/mmdbio
cd mmdbio
go build -o mmdbio .
```

You can now use `./mmdbio` to run the CLI.

---

### 3. Download Pre-Built Binaries

##### Overview
These are prebuilt binaries for the `mmdbio` tool. Users can download these files directly from GitHub Releases without needing to build from source.

The tool allows working with MaxMind DB files (MMDB) for geolocation purposes, including reading, converting, and analyzing `.mmdb` files.

##### Prebuilt Binaries

| Platform | Architecture | File Name |
|----------|-------------|-----------|
| Linux    | amd64       | mmdbio-1.0.0-linux-amd64.tar.gz |
| Linux    | arm64       | mmdbio-1.0.0-linux-arm64.tar.gz |
| macOS    | amd64       | mmdbio-1.0.0-darwin-amd64.tar.gz |
| macOS    | arm64       | mmdbio-1.0.0-darwin-arm64.tar.gz |
| Windows  | amd64       | mmdbio-1.0.0-windows-amd64.zip |

---

#### Installation Instructions

##### 1. Linux
1. Download the `.tar.gz` file for your architecture.
2. Extract it to a folder in your PATH, e.g., `/usr/local/bin`:

```bash
tar -xzf mmdbio-1.0.0-linux-amd64.tar.gz -C /usr/local/bin
```

3. Rename the binary for simplicity:

```bash
mv /usr/local/bin/mmdbio-1.0.0-linux-amd64 /usr/local/bin/mmdbio
```

4. Make the binary executable:

```bash
chmod +x /usr/local/bin/mmdbio
```

5. Verify installation:

```bash
mmdbio --help
```

##### 2. macOS
1. Download the `.tar.gz` file for your architecture (amd64 or arm64).
2. Extract to a folder in your PATH, e.g., `/usr/local/bin`:

```bash
tar -xzf mmdbio-1.0.0-darwin-amd64.tar.gz -C /usr/local/bin
```

3. Rename the binary:

```bash
mv /usr/local/bin/mmdbio-1.0.0-darwin-amd64 /usr/local/bin/mmdbio
```

4. Make executable:

```bash
chmod +x /usr/local/bin/mmdbio
```

5. Verify installation:

```bash
mmdbio --help
```

##### 3. Windows
1. Download the `.zip` file.
2. Extract the `mmdbio-1.0.0-windows-amd64.exe` to a folder included in your system `PATH`.
3. Rename the binary to `mmdbio.exe` for convenience.
4. Open Command Prompt and verify:

```cmd
mmdbio --help
```

---

##### Notes
- Ensure execution permissions on Linux/macOS.
- Recommended folder for binaries: `/usr/local/bin` or any folder in your PATH.
- For updates, check GitHub Releases.
- If `go install` fails due to proxy caching, use:

```bash
GOPROXY=direct go install github.com/IPGeolocation/mmdbio@latest
```

---

##### Troubleshooting
- **Command not found:** Ensure the binary is in a folder included in your PATH.
- **Execution permission error:** Run `chmod +x <binary>` on Linux/macOS.
- **Wrong architecture:** Download the binary matching your OS and CPU architecture.


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

### completion

The `completion` command generates shell completion scripts for `mmdbio`, making it easier to use with Bash, Zsh, Fish, and PowerShell.

**Usage**

```bash
mmdbio completion [bash|zsh|fish|powershell]
```

**Available Shells**

- `bash`  
- `zsh`  
- `fish`  
- `powershell`  

 **Examples**

***Bash***

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

 ***Zsh***

Load completions:

```bash
echo "autoload -U compinit; compinit" >> ~/.zshrc
mmdbio completion zsh > "${fpath[1]}/_mmdbio"
```

***Fish***

Load completions:

```bash
mmdbio completion fish | source
```

Install completions permanently:

```bash
mmdbio completion fish > ~/.config/fish/completions/mmdbio.fish
```

***PowerShell***

Load completions:

```powershell
mmdbio completion powershell | Out-String | Invoke-Expression
```

Install completions permanently:

```powershell
mmdbio completion powershell > mmdbio.ps1
```

**Notes**

- Ensure that you use the appropriate file path for your shell when installing completions permanently.
- The `completion` command supports exactly **one argument**, which must be one of the valid shells listed above.

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
