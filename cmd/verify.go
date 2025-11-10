package cmd

import (
	"fmt"
	"os"

	"github.com/oschwald/maxminddb-golang"
	"github.com/spf13/cobra"
)

var verifyDBPath string

var verifyCmd = &cobra.Command{
	Use:   "verify",
	Short: "Verify the validity of an MMDB file",
	Long:  "Verify that an MMDB file is valid and readable. Returns exit code 0 if valid, 1 if invalid.",
	Run: func(cmd *cobra.Command, args []string) {
		if verifyDBPath == "" {
			fmt.Println("Error: --db flag is required")
			_ = cmd.Help()
			os.Exit(1)
		}

		db, err := maxminddb.Open(verifyDBPath)
		if err != nil {
			fmt.Printf("invalid: failed to open MMDB: %v\n", err)
			os.Exit(1)
		}
		defer db.Close()

		err = db.Verify()
		if err != nil {
			fmt.Printf("invalid: %v\n", err)
			os.Exit(1)
		}

		fmt.Println("valid")
		os.Exit(0)
	},
}

func init() {
	rootCmd.AddCommand(verifyCmd)

	verifyCmd.Flags().StringVar(&verifyDBPath, "db", "", "Path to the .mmdb file")
}
