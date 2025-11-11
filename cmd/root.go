/*
Copyright © 2025 IPGeolocation <support@ipgeolocation.io>
*/
package cmd

import (
	"os"

	"github.com/spf13/cobra"
)

// rootCmd represents the base command when called without any subcommands
var rootCmd = &cobra.Command{
	Use:     "mmdbio",
	Short:   "MMDB file reader utility",
	Version: "1.1.0",
	Long: `
This application is a tool to read and manipulate MMDB files. It can be used to read IP data from an MMDB file, export all records from an MMDB file to JSON, compare two MMDB files, and verify the validity of an MMDB file.`,
}

func Execute() {
	err := rootCmd.Execute()
	if err != nil {
		os.Exit(1)
	}
}

func init() {

	rootCmd.Flags().BoolP("toggle", "t", false, "Help message for toggle")
}
