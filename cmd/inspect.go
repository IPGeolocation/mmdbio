package cmd

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"reflect"
	"strings"

	"github.com/oschwald/maxminddb-golang"
	"github.com/spf13/cobra"
)

var (
	inspectDBPath string
	sampleIP      string
	outputPath    string
	schemaMap     = make(map[string]string)
)

var inspectCmd = &cobra.Command{
	Use:   "inspect",
	Short: "Inspect MMDB structure and optionally export schema to JSON",
	Run: func(cmd *cobra.Command, args []string) {
		if inspectDBPath == "" {
			fmt.Println("Error: --db flag is required")
			_ = cmd.Help()
			os.Exit(1)
		}

		// Open DB
		db, err := maxminddb.Open(inspectDBPath)
		if err != nil {
			log.Fatalf("Failed to open MMDB: %v", err)
		}
		defer db.Close()

		// Parse IP (fallback to Google DNS)
		ip := net.ParseIP(sampleIP)
		if ip == nil {
			ip = net.ParseIP("4.7.229.0")
		}

		// Lookup
		var record interface{}
		if err := db.Lookup(ip, &record); err != nil {
			log.Fatalf("Lookup failed: %v", err)
		}
		if record == nil {
			fmt.Println("No record found for this IP.")
			return
		}

		fmt.Println("📂 Structure for MMDB:", strings.Split(inspectDBPath, "/")[len(strings.Split(inspectDBPath, "/"))-1])
		fmt.Println("────────────────────────────────────────────")

		explore("", record)

		// Print to console
		for k, v := range schemaMap {
			fmt.Printf("%-50s : %s\n", k, v)
		}

		// Export if requested
		if outputPath != "" {
			saveSchemaJSON(outputPath, schemaMap)
		}
	},
}

func explore(prefix string, data interface{}) {
	switch v := data.(type) {
	case map[string]interface{}:
		for key, val := range v {
			path := key
			if prefix != "" {
				path = prefix + "." + key
			}
			explore(path, val)
		}
	case []interface{}:
		for i, val := range v {
			explore(fmt.Sprintf("%s[%d]", prefix, i), val)
		}
	default:
		typeName := "unknown"
		if v != nil {
			typeName = reflect.TypeOf(v).String()
		}
		schemaMap[prefix] = typeName
	}
}

func saveSchemaJSON(path string, schema map[string]string) {
	out, err := json.MarshalIndent(schema, "", "  ")
	if err != nil {
		log.Fatalf("Failed to encode schema: %v", err)
	}

	// Ensure directory exists
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		log.Fatalf("Failed to create directories: %v", err)
	}

	if err := os.WriteFile(path, out, 0644); err != nil {
		log.Fatalf("Failed to write schema file: %v", err)
	}

	fmt.Printf("\n✅ Schema exported to: %s\n", path)
}

func init() {
	rootCmd.AddCommand(inspectCmd)
	inspectCmd.Flags().StringVar(&inspectDBPath, "db", "", "Path to the .mmdb file")
	inspectCmd.Flags().StringVar(&sampleIP, "sample-ip", "", "Sample IP to inspect structure")
	inspectCmd.Flags().StringVar(&outputPath, "out", "", "Optional path to export schema as JSON")
}
