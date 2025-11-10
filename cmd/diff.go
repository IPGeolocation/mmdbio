package cmd

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"reflect"

	"github.com/oschwald/maxminddb-golang"
	"github.com/spf13/cobra"
)

var (
	oldPath string
	newPath string
	summary bool
	jsonOut bool
)

var diffCmd = &cobra.Command{
	Use:   "diff",
	Short: "Compare two MMDB files and show differences",
	Long: `Compares two MaxMind MMDB files and lists networks that were added, removed, or modified.
Example:
  mmdb diff --old old.mmdb --new new.mmdb
  mmdb diff --old old.mmdb --new new.mmdb --summary
  mmdb diff --old old.mmdb --new new.mmdb --json`,
	RunE: func(cmd *cobra.Command, args []string) error {
		if oldPath == "" || newPath == "" {
			return fmt.Errorf("both --old and --new flags are required")
		}

		oldDB, err := maxminddb.Open(oldPath)
		if err != nil {
			return fmt.Errorf("failed to open old db: %v", err)
		}
		defer oldDB.Close()

		newDB, err := maxminddb.Open(newPath)
		if err != nil {
			return fmt.Errorf("failed to open new db: %v", err)
		}
		defer newDB.Close()

		oldData := make(map[string]interface{})
		newData := make(map[string]interface{})

		// --- Read old database ---
		oldIter := oldDB.Networks()
		for oldIter.Next() {
			var record map[string]interface{}
			network, err := oldIter.Network(&record)
			if err == nil {
				oldData[network.String()] = record
			}
		}
		if err := oldIter.Err(); err != nil {
			log.Printf("warning: error reading old db: %v\n", err)
		}

		// --- Read new database ---
		newIter := newDB.Networks()
		for newIter.Next() {
			var record map[string]interface{}
			network, err := newIter.Network(&record)
			if err == nil {
				newData[network.String()] = record
			}
		}
		if err := newIter.Err(); err != nil {
			log.Printf("warning: error reading new db: %v\n", err)
		}

		// --- Compare ---
		added := make(map[string]interface{})
		removed := make(map[string]interface{})
		modified := make(map[string]interface{})

		for netStr, oldVal := range oldData {
			newVal, exists := newData[netStr]
			if !exists {
				removed[netStr] = oldVal
			} else if !reflect.DeepEqual(oldVal, newVal) {
				modified[netStr] = map[string]interface{}{
					"old": oldVal,
					"new": newVal,
				}
			}
		}

		for netStr, newVal := range newData {
			if _, exists := oldData[netStr]; !exists {
				added[netStr] = newVal
			}
		}

		// --- Output ---
		if summary {
			fmt.Printf("Added: %d | Removed: %d | Modified: %d\n",
				len(added), len(removed), len(modified))
			return nil
		}

		output := map[string]interface{}{
			"added":    added,
			"removed":  removed,
			"modified": modified,
		}

		if jsonOut {
			enc := json.NewEncoder(os.Stdout)
			enc.SetIndent("", "  ")
			return enc.Encode(output)
		}

		fmt.Printf("Added: %d\nRemoved: %d\nModified: %d\n\n",
			len(added), len(removed), len(modified))

		if len(added) > 0 {
			fmt.Println("Added:")
			for n := range added {
				fmt.Println("  ", n)
			}
		}
		if len(removed) > 0 {
			fmt.Println("Removed:")
			for n := range removed {
				fmt.Println("  ", n)
			}
		}
		if len(modified) > 0 {
			fmt.Println("Modified:")
			for n := range modified {
				fmt.Println("  ", n)
			}
		}

		return nil
	},
}

func init() {
	rootCmd.AddCommand(diffCmd)
	diffCmd.Flags().StringVar(&oldPath, "old", "", "Path to the old MMDB file")
	diffCmd.Flags().StringVar(&newPath, "new", "", "Path to the new MMDB file")
	diffCmd.Flags().BoolVar(&summary, "summary", false, "Show only summary counts")
	diffCmd.Flags().BoolVar(&jsonOut, "json", false, "Output the result as JSON")
}
