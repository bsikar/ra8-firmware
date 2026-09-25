// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"regexp"
	"testing"
)

// The reviewed set is a literal on purpose. These properties are what make
// keeping it separate from the Proxmox client's operator allowlist safe rather
// than merely duplicated: every name here is one the client would also accept,
// so a reviewed profile can never name a bridge the runtime check refuses.
func TestReviewedRunnerBridgesStayInsideTheClientRule(t *testing.T) {
	clientBridgePattern := regexp.MustCompile(`^vmbr[0-9]{1,4}$`)
	if len(reviewedRunnerBridges) == 0 {
		t.Fatal("the reviewed runner bridge set is empty")
	}
	seen := make(map[string]struct{}, len(reviewedRunnerBridges))
	for _, name := range reviewedRunnerBridges {
		if !clientBridgePattern.MatchString(name) {
			t.Fatalf("reviewed bridge %q is not a bridge the Proxmox client would accept", name)
		}
		if name == "vmbr0" {
			t.Fatal("the management bridge is never a reviewed runner bridge")
		}
		if _, duplicate := seen[name]; duplicate {
			t.Fatalf("reviewed bridge %q is listed twice", name)
		}
		seen[name] = struct{}{}
	}
}

// A profile names a bridge from the reviewed set or it is not a profile.
func TestReviewedRunnerBridgeRefusesAnythingOutsideTheSet(t *testing.T) {
	for _, name := range reviewedRunnerBridges {
		if !reviewedRunnerBridge(name) {
			t.Fatalf("reviewed bridge %q refused", name)
		}
	}
	for _, name := range []string{"", "vmbr0", "vmbr1", "vmbr80", "VMBR8", "vmbr8 ", " vmbr8", "vmbr8.100", "br8", "vmbr8,vmbr9"} {
		if reviewedRunnerBridge(name) {
			t.Fatalf("unreviewed bridge %q accepted", name)
		}
	}
}
