// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func validProfileFixture() Profile {
	return Profile{
		SchemaVersion: ProfileVersion, BoardID: "ek-ra8d2", FixtureRevision: "fixture-v1",
		RestorePolicy: "baseline:sha256:abc123",
		Identity: []SignalCheck{
			{Name: "board-id", Source: "sysfs", Target: "bus/usb/001/serial", Expected: "RA8D2-001"},
			{Name: "probe-id", Source: "sysfs", Target: "bus/usb/002/serial", Expected: "JLINK-001"},
		},
		State: []SignalCheck{
			{Name: "board-power", Source: "sysfs", Target: "class/gpio/board_power/value", Expected: "1"},
			{Name: "reset", Source: "sysfs", Target: "class/gpio/reset/value", Expected: "1"},
			{Name: "relay", Source: "tapo", Target: "board.power_state", Expected: "off"},
		},
		Sensors: []RangeCheck{
			{Name: "vtref-millivolts", Source: "sysfs", Target: "class/hwmon/hwmon0/in0_input", Min: 0, Max: 50},
		},
		ProtectedDevices:   []string{"bus/usb/001/002"},
		ProtectedProcesses: []string{"custom-flasher"},
	}
}

func TestValidateProfileRequiresCompleteFixtureEvidence(t *testing.T) {
	if err := ValidateProfile(validProfileFixture()); err != nil {
		t.Fatalf("valid fixture profile rejected: %v", err)
	}
	for name, edit := range map[string]func(*Profile){
		"missing board identity":   func(p *Profile) { p.Identity = p.Identity[1:] },
		"missing probe identity":   func(p *Profile) { p.Identity = p.Identity[:1] },
		"missing power state":      func(p *Profile) { p.State = p.State[1:] },
		"missing reset state":      func(p *Profile) { p.State = p.State[:1] },
		"missing relay state":      func(p *Profile) { p.State = p.State[:2] },
		"missing VTref sensor":     func(p *Profile) { p.Sensors[0].Name = "temperature" },
		"unsafe signal path":       func(p *Profile) { p.Identity[0].Target = "../../etc/passwd" },
		"executable signal source": func(p *Profile) { p.State[0].Source = "shell" },
		"arbitrary Tapo target":    func(p *Profile) { p.State[2].Target = "pi.power_state" },
		"device traversal":         func(p *Profile) { p.ProtectedDevices[0] = "../ttyACM0" },
		"reversed sensor bounds":   func(p *Profile) { p.Sensors[0].Min = 100 },
		"duplicate process":        func(p *Profile) { p.ProtectedProcesses = []string{"rfp-cli", "RFP-CLI"} },
	} {
		t.Run(name, func(t *testing.T) {
			profile := validProfileFixture()
			edit(&profile)
			if err := ValidateProfile(profile); !errors.Is(err, ErrInvalidProfile) {
				t.Fatalf("invalid profile accepted or wrong error: %v", err)
			}
		})
	}
}

func TestLoadProfileReturnsExactDigestAndRejectsUnsafeFiles(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "neutral.json")
	raw, err := json.Marshal(validProfileFixture())
	if err != nil {
		t.Fatal(err)
	}
	raw = append(raw, '\n')
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
	profile, digest, err := LoadProfile(path)
	if err != nil || profile.BoardID != "ek-ra8d2" {
		t.Fatalf("profile load = %+v, %q, %v", profile, digest, err)
	}
	sum := sha256.Sum256(raw)
	if digest != hex.EncodeToString(sum[:]) {
		t.Fatalf("digest=%s, want exact-file digest %x", digest, sum)
	}
	if err := os.Chmod(path, 0660); err != nil {
		t.Fatal(err)
	}
	if _, _, err := LoadProfile(path); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("group-writable profile accepted: %v", err)
	}
	if err := os.Chmod(path, 0600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(dir, "linked.json")
	if err := os.Symlink(path, link); err != nil {
		t.Fatal(err)
	}
	if _, _, err := LoadProfile(link); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("symlinked profile accepted: %v", err)
	}
	if err := os.WriteFile(path, append(raw, []byte("{}")...), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := LoadProfile(path); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("trailing profile data accepted: %v", err)
	}
	duplicateRaw := strings.Replace(string(raw), `"schema_version":1,`, `"schema_version":1,"schema_version":1,`, 1)
	if err := os.WriteFile(path, []byte(duplicateRaw), 0600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := LoadProfile(path); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("duplicate profile field accepted: %v", err)
	}
	if strings.Contains(digest, " ") {
		t.Fatalf("unexpected digest encoding %q", digest)
	}
}
