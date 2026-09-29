// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

// A HIL attempt arrives carrying the timing decision it was planned under,
// and the plane re-judges that evidence rather than trusting it: the workload
// it names has to be the reviewed task, the window has to fit inside the
// task's own maximum, and the sample count has to agree with the source the
// decision claims. None of it needs a database.

func hilDefinition() catalog.HILTask {
	return catalog.HILTask{
		BoardID:             "ek~ra8d2",
		BoardModel:          "EK-RA8D2",
		ManifestPath:        "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily:       "uart-demo",
		Mode:                string(hilspec.ModeUARTScrape),
		FlashRestoreSeconds: 45,
	}
}

func hilEvidence() HILTimingEvidence {
	definition := hilDefinition()
	return HILTimingEvidence{
		Workload: hilspec.Workload{
			ManifestPath:    definition.ManifestPath,
			BoardModel:      definition.BoardModel,
			FixtureRevision: "fixture-v1",
			ProfileSHA256:   strings.Repeat("a", 64),
			ProgramFamily:   definition.ProgramFamily,
			Mode:            hilspec.Mode(definition.Mode),
		},
		Decision: hilspec.Decision{
			ValidityWindow:    120 * time.Second,
			FlashRestoreBound: 45 * time.Second,
			SafetyMaximum:     600 * time.Second,
			Source:            "observed",
			Samples:           12,
			RejectedRows:      3,
		},
	}
}

// The evidence a planned attempt carries is accepted whole, and the identity
// half of it has to be the reviewed task rather than merely a well-formed
// workload: a claim naming another board, manifest, program or mode is a
// different task's timing and is refused even though every field is valid.
func TestTimingEvidenceMustNameTheReviewedTask(t *testing.T) {
	if !validateHILTimingEvidence(hilEvidence(), hilDefinition(), 900) {
		t.Fatal("whole timing evidence for the reviewed task was refused")
	}

	for label, bend := range map[string]func(*HILTimingEvidence){
		"another manifest": func(e *HILTimingEvidence) {
			e.Workload.ManifestPath = "examples/ek_ra8d2/hw_validated/hil/other/hil.conf"
		},
		"another board":   func(e *HILTimingEvidence) { e.Workload.BoardModel = "EK-RA8M1" },
		"another program": func(e *HILTimingEvidence) { e.Workload.ProgramFamily = "rtt-demo" },
		"another mode":    func(e *HILTimingEvidence) { e.Workload.Mode = hilspec.ModeRTTScrape },
		"a workload that is not safe at all": func(e *HILTimingEvidence) {
			e.Workload.ProfileSHA256 = strings.Repeat("A", 64)
		},
	} {
		claimed := hilEvidence()
		bend(&claimed)
		if validateHILTimingEvidence(claimed, hilDefinition(), 900) {
			t.Fatalf("timing evidence naming %s was admitted", label)
		}
	}
}

// The validity window is the board time this attempt is asking for, so it is
// held to the task's declared maximum from both ends, and it is quoted in
// whole seconds because that is the unit a lease is granted in.
func TestTheWindowIsHeldToTheTasksOwnMaximum(t *testing.T) {
	atTheBound := hilEvidence()
	atTheBound.Decision.ValidityWindow = 900 * time.Second
	atTheBound.Decision.SafetyMaximum = 900 * time.Second
	if !validateHILTimingEvidence(atTheBound, hilDefinition(), 900) {
		t.Fatal("a window exactly at the task maximum was refused")
	}

	shortest := hilEvidence()
	shortest.Decision.ValidityWindow = time.Second
	if !validateHILTimingEvidence(shortest, hilDefinition(), 900) {
		t.Fatal("a one second window was refused")
	}

	for label, bend := range map[string]func(*hilspec.Decision){
		"a window one second over": func(d *hilspec.Decision) { d.ValidityWindow = 901 * time.Second },
		"no window at all":         func(d *hilspec.Decision) { d.ValidityWindow = 0 },
		"a window below zero":      func(d *hilspec.Decision) { d.ValidityWindow = -time.Second },
		"a window part way through a second": func(d *hilspec.Decision) {
			d.ValidityWindow = 120*time.Second + time.Millisecond
		},
	} {
		claimed := hilEvidence()
		bend(&claimed.Decision)
		if validateHILTimingEvidence(claimed, hilDefinition(), 900) {
			t.Fatalf("%s was admitted", label)
		}
	}

	// A task that declares no maximum has nothing to hold the window to, so
	// the evidence is refused rather than admitted under an absent bound.
	for _, maximum := range []int{0, -1} {
		if validateHILTimingEvidence(hilEvidence(), hilDefinition(), maximum) {
			t.Fatalf("evidence was admitted under a task maximum of %d", maximum)
		}
	}
	if !validateHILTimingEvidence(shortest, hilDefinition(), 1) {
		t.Fatal("a one second window was refused under a one second maximum")
	}
}

// Flash and restore time is reserved separately from the window, and the
// bound carried has to be the reviewed one exactly: a client cannot shorten
// the neutralization it will be held to, nor pad it to book more board time.
func TestTheRestoreBoundIsTheReviewedOne(t *testing.T) {
	for label, bound := range map[string]time.Duration{
		"a shorter restore":           44 * time.Second,
		"a longer restore":            46 * time.Second,
		"no restore at all":           0,
		"a restore below zero":        -45 * time.Second,
		"a restore in the wrong unit": 45 * time.Millisecond,
	} {
		claimed := hilEvidence()
		claimed.Decision.FlashRestoreBound = bound
		if validateHILTimingEvidence(claimed, hilDefinition(), 900) {
			t.Fatalf("%s was admitted against a reviewed 45s", label)
		}
	}

	// A task that declares no flash restore is consistent only with evidence
	// carrying none, which is the same exactness read from the other side.
	definition := hilDefinition()
	definition.FlashRestoreSeconds = 0
	none := hilEvidence()
	none.Decision.FlashRestoreBound = 0
	if !validateHILTimingEvidence(none, definition, 900) {
		t.Fatal("a task declaring no flash restore refused evidence carrying none")
	}
	if validateHILTimingEvidence(hilEvidence(), definition, 900) {
		t.Fatal("a task declaring no flash restore admitted a 45s bound")
	}
}

// The safety maximum is the outer bound on the whole thing, so it can never
// be quoted below the window it is supposed to contain, and it is capped at
// an hour however long the task's own maximum is.
func TestTheSafetyMaximumContainsTheWindow(t *testing.T) {
	touching := hilEvidence()
	touching.Decision.SafetyMaximum = touching.Decision.ValidityWindow
	if !validateHILTimingEvidence(touching, hilDefinition(), 900) {
		t.Fatal("a safety maximum exactly at the window was refused")
	}

	atAnHour := hilEvidence()
	atAnHour.Decision.SafetyMaximum = time.Hour
	if !validateHILTimingEvidence(atAnHour, hilDefinition(), 900) {
		t.Fatal("a safety maximum of exactly an hour was refused")
	}

	for label, maximum := range map[string]time.Duration{
		"a safety maximum under the window": 119 * time.Second,
		"no safety maximum at all":          0,
		"a safety maximum past an hour":     time.Hour + time.Second,
	} {
		claimed := hilEvidence()
		claimed.Decision.SafetyMaximum = maximum
		if validateHILTimingEvidence(claimed, hilDefinition(), 900) {
			t.Fatalf("%s was admitted", label)
		}
	}
}

// The sample count has to agree with the source the decision claims: a
// declared window says it was NOT measured, an observed one says it was, and
// five readings is the line between them. That is what stops a thin sample
// being dressed up as measurement, or a measured window being filed as a
// default nobody has to justify.
func TestTheSampleCountMustAgreeWithItsSource(t *testing.T) {
	for _, source := range []string{"default", "hil.conf"} {
		thin := hilEvidence()
		thin.Decision.Source = source
		thin.Decision.Samples = 4
		if !validateHILTimingEvidence(thin, hilDefinition(), 900) {
			t.Fatalf("a %q window with four readings was refused", source)
		}
		none := hilEvidence()
		none.Decision.Source = source
		none.Decision.Samples = 0
		if !validateHILTimingEvidence(none, hilDefinition(), 900) {
			t.Fatalf("a %q window with no readings was refused", source)
		}
		measured := hilEvidence()
		measured.Decision.Source = source
		measured.Decision.Samples = 5
		if validateHILTimingEvidence(measured, hilDefinition(), 900) {
			t.Fatalf("a %q window carrying five readings was admitted", source)
		}
	}

	for _, source := range []string{"observed", "observed-capped"} {
		measured := hilEvidence()
		measured.Decision.Source = source
		measured.Decision.Samples = 5
		if !validateHILTimingEvidence(measured, hilDefinition(), 900) {
			t.Fatalf("a %q window with five readings was refused", source)
		}
		thin := hilEvidence()
		thin.Decision.Source = source
		thin.Decision.Samples = 4
		if validateHILTimingEvidence(thin, hilDefinition(), 900) {
			t.Fatalf("a %q window with four readings was admitted", source)
		}
	}

	for _, source := range []string{"", "observed_capped", "Observed", "measured", "hil.conf "} {
		claimed := hilEvidence()
		claimed.Decision.Source = source
		if validateHILTimingEvidence(claimed, hilDefinition(), 900) {
			t.Fatalf("a window sourced from %q was admitted", source)
		}
	}
}

// Both counts are bounded, so a decision cannot claim an implausible census
// of readings, and neither can go negative.
func TestTheCountsAreBounded(t *testing.T) {
	atTheBound := hilEvidence()
	atTheBound.Decision.Samples = 10000
	atTheBound.Decision.RejectedRows = 10000
	if !validateHILTimingEvidence(atTheBound, hilDefinition(), 900) {
		t.Fatal("counts exactly at their bound were refused")
	}

	for label, bend := range map[string]func(*hilspec.Decision){
		"samples one over":    func(d *hilspec.Decision) { d.Samples = 10001 },
		"samples below zero":  func(d *hilspec.Decision) { d.Samples = -1 },
		"rejected one over":   func(d *hilspec.Decision) { d.RejectedRows = 10001 },
		"rejected below zero": func(d *hilspec.Decision) { d.RejectedRows = -1 },
	} {
		claimed := hilEvidence()
		bend(&claimed.Decision)
		if validateHILTimingEvidence(claimed, hilDefinition(), 900) {
			t.Fatalf("%s was admitted", label)
		}
	}

	// Rejected rows are counted but never weighed against the source, so a
	// decision may reject more rows than it kept and still be admitted.
	noisy := hilEvidence()
	noisy.Decision.Samples = 5
	noisy.Decision.RejectedRows = 9000
	if !validateHILTimingEvidence(noisy, hilDefinition(), 900) {
		t.Fatal("a measured window that rejected most of its rows was refused")
	}
}
