package cmd

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"time"

	"github.com/oschwald/maxminddb-golang"
	"github.com/spf13/cobra"
)

var (
	statsDBPath string
	statsJSON   bool
)

var statsCmd = &cobra.Command{
	Use:   "stats",
	Short: "Display statistics and metadata of an MMDB file",
	Run: func(cmd *cobra.Command, args []string) {
		if statsDBPath == "" {
			fmt.Println("Error: --db flag is required")
			_ = cmd.Help()
			os.Exit(1)
		}

		db, err := maxminddb.Open(statsDBPath)
		if err != nil {
			log.Fatalf("Failed to open MMDB file: %v", err)
		}
		defer db.Close()

		meta := db.Metadata

		// Gather stats
		countV4, countV6 := 0, 0
		networks := db.Networks()
		for networks.Next() {
			var record interface{}
			ipNet, err := networks.Network(&record)
			if err != nil {
				continue
			}
			if ipNet.IP.To4() != nil {
				countV4++
			} else {
				countV6++
			}
		}
		if err := networks.Err(); err != nil {
			fmt.Printf("Error during scan: %v\n", err)
		}

		stats := map[string]interface{}{
			"database_type": meta.DatabaseType,
			"description":   meta.Description,
			"ip_version":    meta.IPVersion,
			"record_size":   meta.RecordSize,
			"node_count":    meta.NodeCount,
			"build_epoch":   time.Unix(int64(meta.BuildEpoch), 0).UTC().Format(time.RFC3339),
			"languages":     meta.Languages,
			"ipv4_count":    countV4,
			"ipv6_count":    countV6,
			"networks":      countV4 + countV6,
		}

		// JSON output
		if statsJSON {

			out, _ := json.MarshalIndent(stats, "", "  ")
			fmt.Println(string(out))
			return
		}

		// Pretty printed output
		fmt.Println("=====MMDB Statistics========")
		fmt.Println("────────────────────────────────────────────")
		fmt.Printf("Database Type: %s\n", meta.DatabaseType)
		if desc, ok := meta.Description["en"]; ok {
			fmt.Printf("Description:   %s\n", desc)
		}
		fmt.Printf("IP Version:    %d\n", meta.IPVersion)
		fmt.Printf("Record Size:   %d bits\n", meta.RecordSize)
		fmt.Printf("Node Count:    %d\n", meta.NodeCount)
		fmt.Printf("Build Epoch:   %s\n", stats["build_epoch"])
		fmt.Printf("Languages:     %v\n", meta.Languages)
		fmt.Println("────────────────────────────────────────────")
		fmt.Printf("IPv4 Count:    %d\n", countV4)
		fmt.Printf("IPv6 Count:    %d\n", countV6)
		fmt.Printf("Networks:      %d\n", countV4+countV6)
		fmt.Println("────────────────────────────────────────────")
	},
}

func init() {
	rootCmd.AddCommand(statsCmd)
	statsCmd.Flags().StringVar(&statsDBPath, "db", "", "Path to the .mmdb file")
	statsCmd.Flags().BoolVar(&statsJSON, "json", false, "Output in JSON format")
}
