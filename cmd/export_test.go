package cmd

import (
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/maxmind/mmdbwriter"
	"github.com/maxmind/mmdbwriter/mmdbtype"
)

// Builds a small IPv6 database with IPv4 aliasing turned on, using the same
// shape as the IPGeolocation.io security and location files. Adjacent
// networks have different records so mmdbwriter does not merge them.
func writeExportFixture(t *testing.T) string {
	t.Helper()

	tree, err := mmdbwriter.New(mmdbwriter.Options{
		DatabaseType: "mmdbio-test",
		RecordSize:   24,
	})
	if err != nil {
		t.Fatal(err)
	}

	record := func(vpn string, score uint16, country string, providers ...string) mmdbtype.Map {
		names := mmdbtype.Slice{}
		for _, p := range providers {
			names = append(names, mmdbtype.String(p))
		}
		return mmdbtype.Map{
			"is_vpn":             mmdbtype.String(vpn),
			"threat_score":       mmdbtype.Uint16(score),
			"vpn_provider_names": names,
			"location": mmdbtype.Map{
				"country": mmdbtype.Map{"code2": mmdbtype.String(country)},
			},
		}
	}

	networks := []struct {
		cidr   string
		record mmdbtype.Map
	}{
		{"1.0.0.0/25", record("true", 90, "DE", "NordVPN")},
		{"1.0.0.128/25", record("true", 50, "DE", "ProtonVPN")},
		{"1.0.1.0/24", record("false", 0, "FR")},
		{"2a00::/33", record("true", 10, "US")},
		{"2a00:0:8000::/33", record("true", 20, "US")},
	}
	for _, n := range networks {
		_, ipnet, err := net.ParseCIDR(n.cidr)
		if err != nil {
			t.Fatal(err)
		}
		if err := tree.Insert(ipnet, n.record); err != nil {
			t.Fatal(err)
		}
	}

	path := filepath.Join(t.TempDir(), "fixture.mmdb")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if _, err := tree.WriteTo(f); err != nil {
		t.Fatal(err)
	}
	return path
}

// Runs "mmdbio export" with fresh flag values and returns the output file
func runExport(t *testing.T, args ...string) string {
	t.Helper()

	exportDBPath, exportOut, exportRanges = "", "", ""
	exportFields, exportWhere = nil, nil
	exportFormat, exportFamily, exportAggregate = "json", 0, true

	out := filepath.Join(t.TempDir(), "out")
	rootCmd.SetArgs(append([]string{"export", "--out", out}, args...))
	if err := rootCmd.Execute(); err != nil {
		t.Fatal(err)
	}

	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func lines(s string) []string {
	return strings.Fields(s)
}

func TestExportJSONSkipsAliasedNetworks(t *testing.T) {
	db := writeExportFixture(t)

	var records map[string]interface{}
	if err := json.Unmarshal([]byte(runExport(t, "--db", db)), &records); err != nil {
		t.Fatal(err)
	}

	var got []string
	for k := range records {
		got = append(got, k)
	}
	if len(got) != 5 {
		t.Fatalf("expected 5 networks without ::ffff:0:0/96, 2001::/32 or 2002::/16 copies, got %d: %v", len(got), got)
	}
}

func TestExportCIDR(t *testing.T) {
	db := writeExportFixture(t)

	tests := []struct {
		name string
		args []string
		want []string
	}{
		{
			name: "string flag, merged by default",
			args: []string{"--where", "is_vpn=true"},
			want: []string{"1.0.0.0/24", "2a00::/32"},
		},
		{
			name: "aggregate off",
			args: []string{"--where", "is_vpn=true", "--aggregate=false"},
			want: []string{"1.0.0.0/25", "1.0.0.128/25", "2a00::/33", "2a00:0:8000::/33"},
		},
		{
			name: "family 4",
			args: []string{"--where", "is_vpn=true", "--family", "4", "--aggregate=false"},
			want: []string{"1.0.0.0/25", "1.0.0.128/25"},
		},
		{
			name: "family 6",
			args: []string{"--family", "6", "--aggregate=false"},
			want: []string{"2a00::/33", "2a00:0:8000::/33"},
		},
		{
			name: "numeric threshold",
			args: []string{"--where", "threat_score>=80"},
			want: []string{"1.0.0.0/25"},
		},
		{
			name: "nested path, comma list, any case",
			args: []string{"--where", "location.country.code2=fr,us"},
			want: []string{"1.0.1.0/24", "2a00::/32"},
		},
		{
			name: "array element",
			args: []string{"--where", "vpn_provider_names=protonvpn"},
			want: []string{"1.0.0.128/25"},
		},
		{
			name: "not equal",
			args: []string{"--where", "is_vpn!=true"},
			want: []string{"1.0.1.0/24"},
		},
		{
			name: "conditions are ANDed",
			args: []string{"--where", "is_vpn=true", "--where", "threat_score<60", "--family", "4"},
			want: []string{"1.0.0.128/25"},
		},
		{
			name: "missing field never matches",
			args: []string{"--where", "is_tor!=true"},
			want: nil,
		},
		{
			name: "range filter",
			args: []string{"--range", "1.0.1.0/24"},
			want: []string{"1.0.1.0/24"},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			args := append([]string{"--db", db, "--format", "cidr"}, tt.args...)
			got := lines(runExport(t, args...))
			if len(got) != len(tt.want) || (len(got) > 0 && !reflect.DeepEqual(got, tt.want)) {
				t.Errorf("got %v, want %v", got, tt.want)
			}
		})
	}
}

// Records do not have to be maps; some databases store a bare value
func TestExportNonMapRecords(t *testing.T) {
	tree, err := mmdbwriter.New(mmdbwriter.Options{DatabaseType: "mmdbio-test", RecordSize: 24})
	if err != nil {
		t.Fatal(err)
	}
	for cidr, value := range map[string]string{"1.0.0.0/24": "blocked", "1.0.1.0/24": "allowed", "1.0.2.0/24": "blocked"} {
		_, ipnet, _ := net.ParseCIDR(cidr)
		if err := tree.Insert(ipnet, mmdbtype.String(value)); err != nil {
			t.Fatal(err)
		}
	}
	db := filepath.Join(t.TempDir(), "strings.mmdb")
	f, err := os.Create(db)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tree.WriteTo(f); err != nil {
		t.Fatal(err)
	}
	f.Close()

	var records map[string]interface{}
	if err := json.Unmarshal([]byte(runExport(t, "--db", db)), &records); err != nil {
		t.Fatal(err)
	}
	want := map[string]interface{}{"1.0.0.0/24": "blocked", "1.0.1.0/24": "allowed", "1.0.2.0/24": "blocked"}
	if !reflect.DeepEqual(records, want) {
		t.Errorf("got %v, want %v", records, want)
	}
}

func TestParseCondition(t *testing.T) {
	tests := []struct {
		in      string
		want    condition
		wantErr bool
	}{
		{in: "is_vpn=true", want: condition{path: "is_vpn", op: "=", values: []string{"true"}}},
		{in: "location.country.code2=DE, FR", want: condition{path: "location.country.code2", op: "=", values: []string{"DE", "FR"}}},
		{in: "is_tor!=true", want: condition{path: "is_tor", op: "!=", values: []string{"true"}}},
		{in: "threat_score>=80", want: condition{path: "threat_score", op: ">=", number: 80}},
		{in: "threat_score<20.5", want: condition{path: "threat_score", op: "<", number: 20.5}},
		{in: "asn.organization=A=B", want: condition{path: "asn.organization", op: "=", values: []string{"A=B"}}},
		{in: "is_vpn", wantErr: true},
		{in: "=true", wantErr: true},
		{in: "is_vpn=", wantErr: true},
		{in: "threat_score>=high", wantErr: true},
		{in: "threat_score!80", wantErr: true},
	}

	for _, tt := range tests {
		got, err := parseCondition(tt.in)
		if tt.wantErr {
			if err == nil {
				t.Errorf("parseCondition(%q): expected an error, got %+v", tt.in, got)
			}
			continue
		}
		if err != nil {
			t.Errorf("parseCondition(%q): %v", tt.in, err)
			continue
		}
		if !reflect.DeepEqual(got, tt.want) {
			t.Errorf("parseCondition(%q) = %+v, want %+v", tt.in, got, tt.want)
		}
	}
}
