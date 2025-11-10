package cmd

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strings"

	"github.com/oschwald/maxminddb-golang"
	"github.com/spf13/cobra"
)

var (
	dbPath    string
	ipAddr    string
	fields    []string
	inputPath string
	output    string
	ipRange   string
)

var readCmd = &cobra.Command{
	Use:   "read",
	Short: "Read IP data from an MMDB file (supports single, batch, or CIDR range mode)",
	Run: func(cmd *cobra.Command, args []string) {
		if dbPath == "" {
			fmt.Println("Error: --db flag is required")
			_ = cmd.Help()
			os.Exit(1)
		}

		db, err := maxminddb.Open(dbPath)
		if err != nil {
			log.Fatalf("Failed to open MMDB file: %v", err)
		}
		defer db.Close()

		results := make(map[string]interface{})

		// Case 1: batch mode (input file or stdin)
		if inputPath != "" {
			ips, err := readIPsFromInput(inputPath)
			if err != nil {
				log.Fatalf("Failed to read IPs: %v", err)
			}
			processIPs(db, ips, results)
			writeResults(results)
			return
		}

		// Case 2: range mode
		if ipRange != "" {
			ips, err := expandCIDR(ipRange)
			if err != nil {
				log.Fatalf("Invalid CIDR range: %v", err)
			}
			processIPs(db, ips, results)
			writeResults(results)
			return
		}

		// Case 3: single lookup
		if ipAddr == "" {
			fmt.Println("Error: either --ip, --range, or --input is required")
			os.Exit(1)
		}

		ip := net.ParseIP(ipAddr)
		if ip == nil {
			log.Fatalf("Invalid IP address: %s", ipAddr)
		}

		var record interface{}
		err = db.Lookup(ip, &record)
		if err != nil {
			log.Fatalf("Lookup error: %v", err)
		}

		if record == nil {
			fmt.Println("No data found for that IP.")
			return
		}

		// If --fields is provided, extract them
		if len(fields) > 0 {
			fieldResults := make(map[string]interface{})
			for _, f := range fields {
				val, ok := extractField(record, f)
				if ok {
					fieldResults[f] = val
				} else {
					fieldResults[f] = nil
				}
			}
			results[ipAddr] = fieldResults
			writeResults(results)
			return
		}

		// Default: print full record
		out, _ := json.MarshalIndent(record, "", "  ")
		fmt.Println(string(out))
	},
}

// Reads IPs line by line from a file or stdin
func readIPsFromInput(path string) ([]string, error) {
	var reader io.Reader
	if path == "-" {
		reader = os.Stdin
	} else {
		file, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		defer file.Close()
		reader = file
	}

	var ips []string
	scanner := bufio.NewScanner(reader)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line != "" {
			ips = append(ips, line)
		}
	}
	return ips, scanner.Err()
}

// Writes results to stdout or a JSON file
func writeResults(results map[string]interface{}) {
	if output != "" {
		data, _ := json.MarshalIndent(results, "", "  ")
		err := os.WriteFile(output, data, 0644)
		if err != nil {
			log.Fatalf("Failed to write output: %v", err)
		}
		fmt.Printf("Results saved to %s\n", output)
		return
	}

	out, _ := json.MarshalIndent(results, "", "  ")
	fmt.Println(string(out))
}

// Extracts a nested field like "location.country.name"
func extractField(data interface{}, path string) (interface{}, bool) {
	keys := strings.Split(path, ".")
	current := data

	for _, key := range keys {
		switch node := current.(type) {
		case map[string]interface{}:
			val, exists := node[key]
			if !exists {
				return nil, false
			}
			current = val
		default:
			return nil, false
		}
	}

	return current, true
}

// Expands a CIDR (e.g. 192.168.0.0/30) into a list of IP addresses
func expandCIDR(cidr string) ([]string, error) {
	ip, ipnet, err := net.ParseCIDR(cidr)
	if err != nil {
		return nil, err
	}

	var ips []string
	for ip := ip.Mask(ipnet.Mask); ipnet.Contains(ip); incIP(ip) {
		ips = append(ips, ip.String())
	}
	return ips, nil
}

// Increment an IP (IPv4 or IPv6)
func incIP(ip net.IP) {
	for j := len(ip) - 1; j >= 0; j-- {
		ip[j]++
		if ip[j] > 0 {
			break
		}
	}
}

// Lookup for multiple IPs
func processIPs(db *maxminddb.Reader, ips []string, results map[string]interface{}) {
	for _, ipStr := range ips {
		ip := net.ParseIP(strings.TrimSpace(ipStr))
		if ip == nil {
			results[ipStr] = "invalid_ip"
			continue
		}

		var record interface{}
		err := db.Lookup(ip, &record)
		if err != nil {
			results[ipStr] = fmt.Sprintf("lookup_error: %v", err)
			continue
		}
		if record == nil {
			results[ipStr] = nil
			continue
		}

		// Extract only requested fields if specified
		if len(fields) > 0 {
			fieldMap := make(map[string]interface{})
			for _, f := range fields {
				val, ok := extractField(record, f)
				if ok {
					fieldMap[f] = val
				} else {
					fieldMap[f] = nil
				}
			}
			results[ipStr] = fieldMap
		} else {
			results[ipStr] = record
		}
	}
}

func init() {
	rootCmd.AddCommand(readCmd)

	readCmd.Flags().StringVar(&dbPath, "db", "", "Path to the .mmdb file")
	readCmd.Flags().StringVar(&ipAddr, "ip", "", "IP address to lookup")
	readCmd.Flags().StringSliceVar(&fields, "fields", nil, "Comma-separated list of fields to extract (e.g. location.country.name,city.names.en)")
	readCmd.Flags().StringVar(&inputPath, "input", "", "Path to file with IPs (or '-' for stdin)")
	readCmd.Flags().StringVar(&output, "out", "", "Optional output file path")
	readCmd.Flags().StringVar(&ipRange, "range", "", "CIDR range (e.g. 192.168.1.0/24) to lookup all IPs in range")
}
