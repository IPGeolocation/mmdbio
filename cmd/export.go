package cmd

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"strings"

	"github.com/oschwald/maxminddb-golang"
	"github.com/spf13/cobra"
)

var (
	exportDBPath string
	exportOut    string
	exportFields []string
	exportRanges string
)

var exportCmd = &cobra.Command{
	Use:   "export",
	Short: "Export all records from an MMDB to JSON (supports optional field and range filtering)",
	Run: func(cmd *cobra.Command, args []string) {
		if exportDBPath == "" || exportOut == "" {
			fmt.Println("Error: both --db and --out are required")
			_ = cmd.Help()
			os.Exit(1)
		}

		db, err := maxminddb.Open(exportDBPath)
		if err != nil {
			log.Fatalf("Failed to open MMDB: %v", err)
		}
		defer db.Close()

		// Parse ranges if provided
		var filterRanges []*net.IPNet
		if exportRanges != "" {
			for _, cidr := range strings.Split(exportRanges, ",") {
				_, ipnet, err := net.ParseCIDR(strings.TrimSpace(cidr))
				if err != nil {
					log.Fatalf("Invalid range '%s': %v", cidr, err)
				}
				filterRanges = append(filterRanges, ipnet)
			}
		}

		allRecords := make(map[string]interface{})

		// Get iterator
		networks := db.Networks()

		for networks.Next() {
			var record interface{}
			network, err := networks.Network(&record)
			if err != nil {
				log.Printf("Warning: failed to decode network: %v", err)
				continue
			}

			// Apply range filter
			if len(filterRanges) > 0 && !isNetworkInRanges(network, filterRanges) {
				continue
			}

			// Apply field extraction (single or multiple)
			if len(exportFields) > 0 {
				fieldMap := make(map[string]interface{})
				for _, f := range exportFields {
					val, ok := extractField(record, f)
					if ok {
						fieldMap[f] = val
					} else {
						fieldMap[f] = nil
					}
				}
				allRecords[network.String()] = fieldMap
			} else {
				allRecords[network.String()] = record
			}
		}

		if err := networks.Err(); err != nil {
			log.Fatalf("Error iterating networks: %v", err)
		}

		// Save JSON
		data, err := json.MarshalIndent(allRecords, "", "  ")
		if err != nil {
			log.Fatalf("Failed to marshal JSON: %v", err)
		}

		if err := os.WriteFile(exportOut, data, 0644); err != nil {
			log.Fatalf("Failed to write JSON file: %v", err)
		}

		fmt.Printf("Exported %d records to %s\n", len(allRecords), exportOut)
	},
}

func isNetworkInRanges(network *net.IPNet, ranges []*net.IPNet) bool {
	for _, r := range ranges {
		if networkOverlap(network, r) {
			return true
		}
	}
	return false
}

// Returns true if network a overlaps network b
func networkOverlap(a, b *net.IPNet) bool {
	return a.Contains(b.IP) || b.Contains(a.IP)
}

func init() {
	rootCmd.AddCommand(exportCmd)
	exportCmd.Flags().StringVar(&exportDBPath, "db", "", "Path to the .mmdb file")
	exportCmd.Flags().StringVar(&exportOut, "out", "", "Path to output JSON file")
	exportCmd.Flags().StringSliceVar(&exportFields, "fields", nil, "Comma-separated list of fields to extract (e.g. location.country.name,city.names.en)")
	exportCmd.Flags().StringVar(&exportRanges, "range", "", "Optional comma-separated CIDR ranges to filter networks")
}
