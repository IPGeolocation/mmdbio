package cmd

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"strings"

	"github.com/maxmind/mmdbwriter"
	"github.com/maxmind/mmdbwriter/inserter"
	"github.com/maxmind/mmdbwriter/mmdbtype"
	"github.com/spf13/cobra"
)

// Flags for the import command.
var (
	inPath            string
	outPath           string
	ipVersion         int
	recordSize        int
	mergeStrategyName string
	alias6to4         bool
	disallowReserved  bool
	title             string
	description       string
)

// importCmd represents the "import" command.
var importCmd = &cobra.Command{
	Use:   "import",
	Short: "Import JSON data and generate a MaxMind .mmdb file",
	Long: `
The "import" command reads an exported JSON file (produced by your CLI's 'export' command)
and converts it into a fully functional MaxMind-style .mmdb database for IP lookups.

───────────────────────────────
📦 SUPPORTED INPUT FORMAT
───────────────────────────────
The import command expects JSON in this format:

{
  "1.0.133.89/32": {
    "is_proxy": "true",
    "is_known_attacker": "true",
    "proxy_type": "VPN",
    "threat_score": 75
  },
  "1.0.141.70/32": {
    "is_proxy": "true",
    "is_known_attacker": "true",
    "proxy_type": "VPN",
    "threat_score": 75
  }
}

Each key must be:
  • A CIDR block, e.g. "1.2.3.0/24"
  • A single IP address, e.g. "8.8.8.8"
  • Or an IP range, e.g. "1.2.3.0-1.2.3.255"

───────────────────────────────
 OPTIONS OVERVIEW
───────────────────────────────
Flags available for customization:

  --in, -i                 Input JSON file path
  --out, -o                Output .mmdb file path
  --ip                     IP version (4 or 6) [default: 6]
  --size                   Record size (24, 28, or 32) [default: 32]
  --merge                  Merge strategy for duplicate entries
                           Options: none, toplevel, recurse
  --alias-6to4             Enable IPv6 to IPv4 aliasing (for hybrid DBs)
  --disallow-reserved       Skip reserved IP blocks (e.g. 127.0.0.0/8)

───────────────────────────────
 BEHIND THE SCENES
───────────────────────────────
Internally, this command:
  1. Parses your JSON into memory.
  2. Converts Go native types → MaxMind mmdbtype (string, bool, float, map, slice, etc.)
  3. Inserts each CIDR or range into a mmdbwriter tree.
  4. Serializes that tree into a valid .mmdb file.

───────────────────────────────
 EXAMPLES
───────────────────────────────

 Import IPv4 data
-------------------
$ mmdbio import \
  --in ipv4_export.json \
  --out ipv4_data.mmdb \
  --ip 4 \
  --size 32

 Import IPv6 data
-------------------
$ mmdbio import \
  --in ipv6_export.json \
  --out ipv6_data.mmdb \
  --ip 6 \
  --size 28

Import mixed data (with 6to4 aliasing)
----------------------------------------
$ mmdbio import \
  --in all_data.json \
  --out all.mmdb \
  --ip 6 \
  --alias-6to4

Import and skip reserved IPs
-------------------------------
$ mmdbio import \
  --in dataset.json \
  --out filtered.mmdb \
  --disallow-reserved

───────────────────────────────
🔍  OUTPUT
───────────────────────────────
Upon successful import, you’ll see:
   Successfully wrote 2048 entries to ./output.mmdb

───────────────────────────────
  NOTES
───────────────────────────────
• The output .mmdb file can be queried with any MaxMind-compatible reader.
• Nested maps and arrays are supported (automatically converted).
• Duplicate ranges are handled according to the --merge flag.
• For debugging invalid entries, warnings are printed to stderr.

───────────────────────────────
  EXAMPLE JSON (IPv6)
───────────────────────────────
{
  "::100:8559/128": {
    "is_proxy": "true",
    "proxy_type": "VPN",
    "threat_score": 90
  },
  "::100:8d46/128": {
    "is_proxy": "true",
    "proxy_type": "Hosting",
    "threat_score": 70
  }
}
`,
	RunE: func(cmd *cobra.Command, args []string) error {
		// Validate input and output
		if inPath == "" {
			return fmt.Errorf("--in is required")
		}
		if outPath == "" {
			return fmt.Errorf("--out is required")
		}

		// Validate IP version
		if ipVersion != 4 && ipVersion != 6 {
			return fmt.Errorf("--ip must be 4 or 6")
		}

		// Validate record size
		if recordSize != 24 && recordSize != 28 && recordSize != 32 {
			return fmt.Errorf("--size must be 24, 28, or 32")
		}

		// Select merge strategy
		var mergeStrategy inserter.FuncGenerator
		switch mergeStrategyName {
		case "none":
			mergeStrategy = inserter.ReplaceWith
		case "toplevel":
			mergeStrategy = inserter.TopLevelMergeWith
		case "recurse":
			mergeStrategy = inserter.DeepMergeWith
		default:
			return fmt.Errorf("--merge must be one of: none, toplevel, recurse")
		}

		// Open input file
		file, err := os.Open(inPath)
		if err != nil {
			return fmt.Errorf("failed to open input: %v", err)
		}
		defer file.Close()

		// Parse JSON into map[string]map[string]interface{}
		data := make(map[string]map[string]interface{})
		if err := json.NewDecoder(file).Decode(&data); err != nil {
			return fmt.Errorf("failed to parse JSON: %v", err)
		}

		if len(data) == 0 {
			return fmt.Errorf("no records found in %s", inPath)
		}

		// Create mmdb writer tree
		tree, err := mmdbwriter.New(mmdbwriter.Options{
			DatabaseType:            title,
			Description:             map[string]string{"en": description},
			Languages:               []string{"en"},
			IPVersion:               ipVersion,
			RecordSize:              recordSize,
			Inserter:                mergeStrategy,
			DisableIPv4Aliasing:     !alias6to4,
			IncludeReservedNetworks: !disallowReserved,
		})
		if err != nil {
			return fmt.Errorf("failed to create mmdb writer: %v", err)
		}

		// Insert records
		count := 0
		for key, fields := range data {
			record := mmdbtype.Map{}
			for field, val := range fields {
				mmdbVal, err := convertToMMDBType(val)
				if err != nil {
					return fmt.Errorf("failed to convert field %q: %v", field, err)
				}
				record[mmdbtype.String(field)] = mmdbVal
			}

			// Determine if it's a CIDR or range
			if strings.Contains(key, "-") {
				parts := strings.Split(key, "-")
				if len(parts) != 2 {
					fmt.Fprintf(os.Stderr, "warn: invalid range %s\n", key)
					continue
				}
				startIP := net.ParseIP(parts[0])
				endIP := net.ParseIP(parts[1])
				if startIP == nil || endIP == nil {
					fmt.Fprintf(os.Stderr, "warn: invalid IPs in %s\n", key)
					continue
				}
				if err := tree.InsertRange(startIP, endIP, record); err != nil {
					fmt.Fprintf(os.Stderr, "warn: could not insert range %s\n", key)
				}
			} else {
				_, network, err := net.ParseCIDR(key)
				if err != nil {
					// maybe it's a single IP without /mask
					if ipVersion == 6 && strings.Contains(key, ":") {
						key += "/128"
					} else {
						key += "/32"
					}
					_, network, err = net.ParseCIDR(key)
					if err != nil {
						fmt.Fprintf(os.Stderr, "warn: invalid network %s\n", key)
						continue
					}
				}
				if err := tree.Insert(network, record); err != nil {
					fmt.Fprintf(os.Stderr, "warn: could not insert %s\n", key)
				}
			}
			count++
		}

		// Write mmdb file
		outFile, err := os.Create(outPath)
		if err != nil {
			return fmt.Errorf("failed to create output: %v", err)
		}
		defer outFile.Close()

		if _, err := tree.WriteTo(outFile); err != nil {
			return fmt.Errorf("failed to write mmdb: %v", err)
		}

		fmt.Fprintf(os.Stderr, "✅ Successfully wrote %d entries to %s\n", count, outPath)
		return nil
	},
}

// convertToMMDBType recursively converts interface{} into mmdbtype.DataType
func convertToMMDBType(value interface{}) (mmdbtype.DataType, error) {
	switch v := value.(type) {
	case nil:
		return mmdbtype.String(""), nil
	case string:
		return mmdbtype.String(v), nil
	case bool:
		return mmdbtype.Bool(v), nil
	case float64:
		return mmdbtype.Float64(v), nil
	case int:
		return mmdbtype.Int32(int32(v)), nil
	case int32:
		return mmdbtype.Int32(v), nil
	case uint32:
		return mmdbtype.Uint32(v), nil
	case map[string]interface{}:
		m := mmdbtype.Map{}
		for k, val := range v {
			conv, err := convertToMMDBType(val)
			if err != nil {
				return nil, err
			}
			m[mmdbtype.String(k)] = conv
		}
		return m, nil
	case []interface{}:
		arr := mmdbtype.Slice{}
		for _, val := range v {
			conv, err := convertToMMDBType(val)
			if err != nil {
				return nil, err
			}
			arr = append(arr, conv)
		}
		return arr, nil
	default:
		return mmdbtype.String(fmt.Sprintf("%v", v)), nil
	}
}

// init registers the import command and flags.
func init() {
	rootCmd.AddCommand(importCmd)

	importCmd.Flags().StringVarP(&inPath, "in", "i", "", "Input JSON file path (from export command)")
	importCmd.Flags().StringVarP(&outPath, "out", "o", "", "Output .mmdb file path")
	importCmd.Flags().IntVar(&ipVersion, "ip", 6, "IP version (4 or 6)")
	importCmd.Flags().IntVar(&recordSize, "size", 32, "Record size (24, 28, or 32)")
	importCmd.Flags().StringVar(&mergeStrategyName, "merge", "none", "Merge strategy: none, toplevel, recurse")
	importCmd.Flags().BoolVar(&alias6to4, "alias-6to4", false, "Enable IPv6 to IPv4 aliasing")
	importCmd.Flags().BoolVar(&disallowReserved, "disallow-reserved", false, "Disallow inserting reserved IP ranges")
	importCmd.Flags().StringVarP(&title, "title", "t", "Custom-ip-database", "Title for the .mmdb file")
	importCmd.Flags().StringVarP(&description, "description", "d", "Custom IP Intelligence Database", "Description for the .mmdb file")
}
