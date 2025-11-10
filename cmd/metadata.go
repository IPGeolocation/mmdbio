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

var metaDBPath string

var metadataCmd = &cobra.Command{
	Use:   "metadata",
	Short: "Show metadata information from an MMDB file",
	Run: func(cmd *cobra.Command, args []string) {
		if metaDBPath == "" {
			fmt.Println("Error: --db flag is required")
			_ = cmd.Help()
			os.Exit(1)
		}

		db, err := maxminddb.Open(metaDBPath)
		if err != nil {
			log.Fatalf("Failed to open MMDB: %v", err)
		}
		defer db.Close()

		meta := db.Metadata

		// Format build date nicely
		buildTime := time.Unix(int64(meta.BuildEpoch), 0).Format(time.RFC1123)

		fmt.Println("MMDB Metadata")
		fmt.Println("-----------------------------")
		fmt.Printf("Database Type: %s\n", meta.DatabaseType)
		fmt.Printf("IP Version:    %d\n", meta.IPVersion)
		fmt.Printf("Record Size:   %d bits\n", meta.RecordSize)
		fmt.Printf("Node Count:    %d\n", meta.NodeCount)
		fmt.Printf("Build Date:    %s\n", buildTime)
		fmt.Printf("Languages:     %v\n", meta.Languages)

		// Pretty print description JSON
		if len(meta.Description) > 0 {
			descJSON, _ := json.MarshalIndent(meta.Description, "  ", "  ")
			fmt.Println("Description:", string(descJSON))
		}
	},
}

func init() {
	rootCmd.AddCommand(metadataCmd)
	metadataCmd.Flags().StringVar(&metaDBPath, "db", "", "Path to the .mmdb file")
}
