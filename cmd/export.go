package cmd

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"os"
	"strconv"
	"strings"

	"github.com/oschwald/maxminddb-golang"
	"github.com/spf13/cobra"
	"go4.org/netipx"
)

var (
	exportDBPath    string
	exportOut       string
	exportFields    []string
	exportRanges    string
	exportWhere     []string
	exportFormat    string
	exportFamily    int
	exportAggregate bool
)

var exportCmd = &cobra.Command{
	Use:   "export",
	Short: "Export records from an MMDB to JSON or a plain CIDR list (supports field, value, and range filtering)",
	Run: func(cmd *cobra.Command, args []string) {
		if exportDBPath == "" || exportOut == "" {
			fmt.Println("Error: both --db and --out are required")
			_ = cmd.Help()
			os.Exit(1)
		}
		if exportFormat != "json" && exportFormat != "cidr" {
			log.Fatalf("Invalid --format '%s': use json or cidr", exportFormat)
		}
		if exportFormat == "cidr" && len(exportFields) > 0 {
			log.Fatalf("--fields cannot be used with --format cidr")
		}
		if exportFamily != 0 && exportFamily != 4 && exportFamily != 6 {
			log.Fatalf("Invalid --family %d: use 4 or 6", exportFamily)
		}

		var conditions []condition
		for _, w := range exportWhere {
			c, err := parseCondition(w)
			if err != nil {
				log.Fatalf("Invalid --where '%s': %v", w, err)
			}
			conditions = append(conditions, c)
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

		var w io.Writer = os.Stdout
		if exportOut != "-" {
			f, err := os.Create(exportOut)
			if err != nil {
				log.Fatalf("Failed to create output file: %v", err)
			}
			defer f.Close()
			w = f
		}
		bw := bufio.NewWriter(w)

		allRecords := make(map[string]interface{})
		var builder netipx.IPSetBuilder
		matched := 0

		// Get iterator. IPv6 databases also expose the IPv4 subtree at
		// ::ffff:0:0/96, 2001::/32 and 2002::/16; skip those copies.
		networks := db.Networks(maxminddb.SkipAliasedNetworks)

		// Many networks share one record (e.g. every German block in a
		// country file), so decode and match each record only once.
		type cachedRecord struct {
			record  interface{}
			matches bool
		}
		cache := make(map[uintptr]cachedRecord)

		for networks.Next() {
			var rec recordOffset
			network, err := networks.Network(&rec)
			if err != nil {
				log.Printf("Warning: failed to decode network: %v", err)
				continue
			}

			// Apply range filter
			if len(filterRanges) > 0 && !isNetworkInRanges(network, filterRanges) {
				continue
			}

			// Apply family filter
			if exportFamily != 0 && networkFamily(network) != exportFamily {
				continue
			}

			cached, ok := cache[rec.offset]
			if !ok {
				if err := db.Decode(rec.offset, &cached.record); err != nil {
					log.Printf("Warning: failed to decode network %s: %v", network, err)
					continue
				}
				// Apply value filters (all must match)
				cached.matches = matchesAll(cached.record, conditions)
				if exportFormat == "cidr" {
					cached.record = nil
				}
				// Files where most records are unique would only fill the
				// cache, so keep it bounded
				if len(cache) >= 1<<20 {
					clear(cache)
				}
				cache[rec.offset] = cached
			}
			if !cached.matches {
				continue
			}
			record := cached.record

			matched++

			if exportFormat == "cidr" {
				if exportAggregate {
					prefix, ok := netipx.FromStdIPNet(network)
					if !ok {
						log.Printf("Warning: skipping invalid network %s", network)
						continue
					}
					builder.AddPrefix(prefix)
				} else {
					fmt.Fprintln(bw, network.String())
				}
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

		dest := exportOut
		if dest == "-" {
			dest = "stdout"
		}
		summary := ""
		switch {
		case exportFormat == "json":
			data, err := json.MarshalIndent(allRecords, "", "  ")
			if err != nil {
				log.Fatalf("Failed to marshal JSON: %v", err)
			}
			bw.Write(data)
			bw.WriteString("\n")
			summary = fmt.Sprintf("Exported %d records to %s", len(allRecords), dest)
		case exportAggregate:
			set, err := builder.IPSet()
			if err != nil {
				log.Fatalf("Failed to aggregate networks: %v", err)
			}
			prefixes := set.Prefixes()
			for _, p := range prefixes {
				fmt.Fprintln(bw, p.String())
			}
			summary = fmt.Sprintf("Exported %d networks as %d CIDRs to %s", matched, len(prefixes), dest)
		default:
			summary = fmt.Sprintf("Exported %d CIDRs to %s", matched, dest)
		}

		if err := bw.Flush(); err != nil {
			log.Fatalf("Failed to write output: %v", err)
		}

		// Keep stdout clean when the export itself goes to stdout
		if exportOut == "-" {
			fmt.Fprintln(os.Stderr, summary)
		} else {
			fmt.Println(summary)
		}
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

// recordOffset captures where a network's record starts without decoding
// it, so records shared by many networks are decoded only once. It uses
// maxminddb's deserializer hook: ShouldSkip sees the offset first and
// returning true skips the record, whatever its type.
type recordOffset struct {
	offset uintptr
}

func (r *recordOffset) ShouldSkip(offset uintptr) (bool, error) {
	r.offset = offset
	return true, nil
}

func (r *recordOffset) StartSlice(uint) error  { return nil }
func (r *recordOffset) StartMap(uint) error    { return nil }
func (r *recordOffset) End() error             { return nil }
func (r *recordOffset) String(string) error    { return nil }
func (r *recordOffset) Float64(float64) error  { return nil }
func (r *recordOffset) Bytes([]byte) error     { return nil }
func (r *recordOffset) Uint16(uint16) error    { return nil }
func (r *recordOffset) Uint32(uint32) error    { return nil }
func (r *recordOffset) Int32(int32) error      { return nil }
func (r *recordOffset) Uint64(uint64) error    { return nil }
func (r *recordOffset) Uint128(*big.Int) error { return nil }
func (r *recordOffset) Bool(bool) error        { return nil }
func (r *recordOffset) Float32(float32) error  { return nil }

// Returns 4 or 6 depending on the network's address family
func networkFamily(n *net.IPNet) int {
	if _, bits := n.Mask.Size(); bits == 32 {
		return 4
	}
	return 6
}

// A --where filter such as "is_vpn=true", "location.country.code2=DE,FR"
// or "threat_score>=80"
type condition struct {
	path   string
	op     string
	values []string // for = and !=
	number float64  // for >, >=, <, <=
}

// Two-character operators are listed first so ">=" is not read as ">"
var conditionOps = []string{">=", "<=", "!=", "=", ">", "<"}

func parseCondition(s string) (condition, error) {
	i := strings.IndexAny(s, "=!<>")
	if i <= 0 {
		return condition{}, fmt.Errorf("expected field<op>value, e.g. is_vpn=true or threat_score>=80")
	}

	path := strings.TrimSpace(s[:i])
	rest := s[i:]
	for _, op := range conditionOps {
		if !strings.HasPrefix(rest, op) {
			continue
		}
		value := strings.TrimSpace(rest[len(op):])
		if value == "" {
			return condition{}, fmt.Errorf("missing value after '%s'", op)
		}

		c := condition{path: path, op: op}
		if op == "=" || op == "!=" {
			for _, v := range strings.Split(value, ",") {
				c.values = append(c.values, strings.TrimSpace(v))
			}
			return c, nil
		}

		n, err := strconv.ParseFloat(value, 64)
		if err != nil {
			return condition{}, fmt.Errorf("'%s' needs a number, got '%s'", op, value)
		}
		c.number = n
		return c, nil
	}

	return condition{}, fmt.Errorf("unknown operator in '%s'", s)
}

// Returns true if the record satisfies every condition. A record that
// does not have the field never matches.
func matchesAll(record interface{}, conditions []condition) bool {
	for _, c := range conditions {
		val, ok := extractField(record, c.path)
		if !ok || val == nil {
			return false
		}

		switch c.op {
		case "=":
			if !valueIn(val, c.values) {
				return false
			}
		case "!=":
			if valueIn(val, c.values) {
				return false
			}
		default:
			n, ok := toNumber(val)
			if !ok {
				return false
			}
			switch c.op {
			case ">=":
				ok = n >= c.number
			case "<=":
				ok = n <= c.number
			case ">":
				ok = n > c.number
			case "<":
				ok = n < c.number
			}
			if !ok {
				return false
			}
		}
	}
	return true
}

// Compares case-insensitively. For arrays (e.g. vpn_provider_names),
// any element may match.
func valueIn(val interface{}, values []string) bool {
	if list, ok := val.([]interface{}); ok {
		for _, item := range list {
			if valueIn(item, values) {
				return true
			}
		}
		return false
	}

	s := fmt.Sprint(val)
	for _, v := range values {
		if strings.EqualFold(s, v) {
			return true
		}
	}
	return false
}

// Converts MMDB numeric types, and numeric strings such as
// "37.38605", to float64
func toNumber(val interface{}) (float64, bool) {
	switch v := val.(type) {
	case float64:
		return v, true
	case float32:
		return float64(v), true
	case int:
		return float64(v), true
	case int32:
		return float64(v), true
	case int64:
		return float64(v), true
	case uint16:
		return float64(v), true
	case uint32:
		return float64(v), true
	case uint64:
		return float64(v), true
	case *big.Int:
		f, _ := new(big.Float).SetInt(v).Float64()
		return f, true
	case string:
		f, err := strconv.ParseFloat(strings.TrimSpace(v), 64)
		return f, err == nil
	}
	return 0, false
}

func init() {
	rootCmd.AddCommand(exportCmd)
	exportCmd.Flags().StringVar(&exportDBPath, "db", "", "Path to the .mmdb file")
	exportCmd.Flags().StringVar(&exportOut, "out", "", "Path to output file, or '-' for stdout")
	exportCmd.Flags().StringSliceVar(&exportFields, "fields", nil, "Comma-separated list of fields to extract (e.g. location.country.name,city.names.en)")
	exportCmd.Flags().StringVar(&exportRanges, "range", "", "Optional comma-separated CIDR ranges to filter networks")
	exportCmd.Flags().StringArrayVar(&exportWhere, "where", nil, "Only export networks whose field matches, e.g. is_vpn=true, location.country.code2=DE,FR or threat_score>=80 (repeatable, all must match)")
	exportCmd.Flags().StringVar(&exportFormat, "format", "json", "Output format: json or cidr (one network per line)")
	exportCmd.Flags().IntVar(&exportFamily, "family", 0, "Only export IPv4 (4) or IPv6 (6) networks")
	exportCmd.Flags().BoolVar(&exportAggregate, "aggregate", true, "With --format cidr, merge adjacent networks into the fewest CIDRs (--aggregate=false keeps networks as stored)")
}
