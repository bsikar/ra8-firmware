// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardsweep

import (
	"strings"
	"testing"
)

func TestAQuietPassSaysNothing(t *testing.T) {
	if (Report{}).Notable() {
		t.Fatal("a pass that found nothing asked to be printed")
	}
}

func TestAPassThatReclaimedNothingIsStillWorthReading(t *testing.T) {
	// Ten expired leases and none reclaimed is a bench that needs looking
	// at. It must never be silent just because the reclaimed count is zero.
	report := Report{Found: 10, Overtaken: 10}
	if !report.Notable() {
		t.Fatal("a pass that found ten expired leases stayed silent")
	}
	line := report.String()
	for _, want := range []string{"found 10", "reclaimed 0", "already reclaimed 10", "failed 0"} {
		if !strings.Contains(line, want) {
			t.Fatalf("report line %q is missing %q", line, want)
		}
	}
}

func TestAFailedReclaimIsReportedSeparatelyFromAnOvertakenOne(t *testing.T) {
	line := Report{Found: 3, Reclaimed: 1, Overtaken: 1, Failed: 1}.String()
	if !strings.Contains(line, "reclaimed 1") || !strings.Contains(line, "already reclaimed 1") ||
		!strings.Contains(line, "failed 1") {
		t.Fatalf("report line %q does not keep the three outcomes apart", line)
	}
	if !(Report{Failed: 1}).Notable() {
		t.Fatal("a pass that failed to reclaim a board stayed silent")
	}
}
