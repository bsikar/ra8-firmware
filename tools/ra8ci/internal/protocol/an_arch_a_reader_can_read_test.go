// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"strings"
	"testing"
	"time"
)

func linuxFacts(arch string) HostFacts {
	return HostFacts{Cores: 4, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
		LoadKind: "linux_load1", OS: "linux", Arch: arch, CapturedAt: time.Now().UTC()}
}

func TestEveryArchitectureGoReportsIsAccepted(t *testing.T) {
	// The names Go builds for today, including the one that is digits alone
	// and the longest one, so the rule is measured against real ports rather
	// than against the two this plane happens to run on.
	for _, arch := range []string{"386", "amd64", "arm", "arm64", "loong64", "mips",
		"mipsle", "mips64", "mips64le", "mips64p32le", "ppc64", "ppc64le", "riscv64",
		"s390x", "sparc64", "wasm"} {
		if !archIsReportable(arch) {
			t.Fatalf("refused a real architecture name: %q", arch)
		}
	}
}

func TestAnEmptyArchitectureIsRefused(t *testing.T) {
	if archIsReportable("") {
		t.Fatal("accepted no architecture at all")
	}
}

func TestAnArchitectureCarryingAControlCharacterIsRefused(t *testing.T) {
	for _, arch := range []string{"amd64\n", "amd\n64", "amd64\x00", "amd64\t", "\x1b[31mamd64"} {
		if archIsReportable(arch) {
			t.Fatalf("accepted a control character in %q", arch)
		}
	}
}

func TestAnArchitectureThatIsNotValidUTF8IsRefused(t *testing.T) {
	if archIsReportable("amd\xff64") {
		t.Fatal("accepted bytes that are not valid UTF-8")
	}
}

func TestSurroundingWhitespaceIsRefused(t *testing.T) {
	for _, arch := range []string{" amd64", "amd64 ", "amd 64", "\u3000amd64"} {
		if archIsReportable(arch) {
			t.Fatalf("accepted whitespace in %q", arch)
		}
	}
}

func TestAnUppercaseOrPunctuatedArchitectureIsRefused(t *testing.T) {
	for _, arch := range []string{"AMD64", "Amd64", "amd-64", "amd_64", "amd.64", "x86/64"} {
		if archIsReportable(arch) {
			t.Fatalf("accepted a name outside the alphabet: %q", arch)
		}
	}
}

func TestAnArchitectureAtTheBoundIsAcceptedAndPastItRefused(t *testing.T) {
	if !archIsReportable(strings.Repeat("a", maxArchBytes)) {
		t.Fatal("refused a name exactly at the bound")
	}
	if archIsReportable(strings.Repeat("a", maxArchBytes+1)) {
		t.Fatal("accepted a name past the bound")
	}
}

func TestAnUnboundedArchitectureIsRefused(t *testing.T) {
	if archIsReportable(strings.Repeat("amd64", 4096)) {
		t.Fatal("accepted a name no reader could read")
	}
}

func TestHostFactsRefuseAnUnreadableArchitecture(t *testing.T) {
	if err := linuxFacts("amd64").Validate(); err != nil {
		t.Fatalf("refused an honest reading: %v", err)
	}
	for _, arch := range []string{"", "amd64\n", "AMD64", strings.Repeat("a", maxArchBytes+1)} {
		if err := linuxFacts(arch).Validate(); err == nil {
			t.Fatalf("host facts accepted %q", arch)
		}
	}
}

func TestEveryMessageCarryingHostFactsRefusesThem(t *testing.T) {
	// The field travels on four messages and each one reaches the plane on a
	// different endpoint, so the refusal has to hold on all of them, not only
	// where the receipt is judged.
	bad := linuxFacts("amd64\x1b]0;x\x07")
	claim := ClaimRequest{SchemaVersion: Version, HostFacts: bad, PollWaitMS: 1000}
	if err := claim.Validate(); err == nil {
		t.Fatal("a claim carried an unreadable architecture")
	}
	id := "01860d0a-1b2c-7def-8123-456789abcdef"
	digest := strings.Repeat("a", 64)
	ack := Ack{SchemaVersion: Version, AssignmentID: id, AttemptID: id, AssignmentVersion: 1,
		FencingToken: 1, CatalogSHA256: digest, SourceSnapshotSHA256: digest, HostFacts: bad}
	if err := ack.Validate(); err == nil {
		t.Fatal("an acknowledgment carried an unreadable architecture")
	}
	beat := Heartbeat{SchemaVersion: Version, AssignmentID: id, AttemptID: id, AssignmentVersion: 1,
		FencingToken: 1, Phase: "executing", HostFacts: bad}
	if err := beat.Validate(); err == nil {
		t.Fatal("a heartbeat carried an unreadable architecture")
	}
}
