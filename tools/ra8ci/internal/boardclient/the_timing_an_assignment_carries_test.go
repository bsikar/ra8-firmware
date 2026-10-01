// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A HIL assignment carries the timing the agent will hold the fixture to,
// so the client checks that evidence against the reviewed task definition
// before it acts on it. Timing that describes another workload, or that
// was derived from too few samples to claim what it claims, is refused
// here rather than on the bench.

func hilDefinition() catalog.HILTask {
	return catalog.HILTask{BoardID: "ek-ra8d2", BoardModel: "ek-ra8d2-fixture",
		ManifestPath: "hil/manifests/blinky.json", ProgramFamily: "blinky", Mode: "observe",
		FlashRestoreSeconds: 30}
}

func hilEvidence() *store.HILTimingEvidence {
	definition := hilDefinition()
	return &store.HILTimingEvidence{
		Workload: hilspec.Workload{ManifestPath: definition.ManifestPath, BoardModel: definition.BoardModel,
			FixtureRevision: "rev-4", ProfileSHA256: "9f" + "00", ProgramFamily: definition.ProgramFamily,
			Mode: hilspec.Mode(definition.Mode)},
		Decision: hilspec.Decision{ValidityWindow: 90 * time.Second,
			FlashRestoreBound: time.Duration(definition.FlashRestoreSeconds) * time.Second,
			SafetyMaximum:     10 * time.Minute, Source: "observed", Samples: 7},
	}
}

func TestHILTimingMustDescribeTheReviewedTask(t *testing.T) {
	definition := hilDefinition()
	if !validHILTimingAssignment(hilEvidence(), definition, 600) {
		t.Fatal("the fixture evidence was refused")
	}

	otherManifest := hilEvidence()
	otherManifest.Workload.ManifestPath = "hil/manifests/other.json"
	otherBoard := hilEvidence()
	otherBoard.Workload.BoardModel = "ek-ra8m1-fixture"
	otherFamily := hilEvidence()
	otherFamily.Workload.ProgramFamily = "throughput"
	otherMode := hilEvidence()
	otherMode.Workload.Mode = hilspec.Mode("measure")
	noFixture := hilEvidence()
	noFixture.Workload.FixtureRevision = ""
	noProfile := hilEvidence()
	noProfile.Workload.ProfileSHA256 = ""
	otherRestore := hilEvidence()
	otherRestore.Decision.FlashRestoreBound = 31 * time.Second

	for name, evidence := range map[string]*store.HILTimingEvidence{
		"no evidence at all":          nil,
		"another manifest":            otherManifest,
		"another board model":         otherBoard,
		"another program family":      otherFamily,
		"another mode":                otherMode,
		"no fixture revision":         noFixture,
		"no profile digest":           noProfile,
		"another flash restore bound": otherRestore,
	} {
		if validHILTimingAssignment(evidence, definition, 600) {
			t.Fatalf("%s was accepted", name)
		}
	}
}

// The validity window is what the agent will actually hold, so it is held
// to whole seconds, to the task's own deadline, and under a safety maximum
// that is itself bounded at an hour.
func TestHILTimingWindowIsBoundedByTheTaskDeadline(t *testing.T) {
	definition := hilDefinition()

	noWindow := hilEvidence()
	noWindow.Decision.ValidityWindow = 0
	negative := hilEvidence()
	negative.Decision.ValidityWindow = -time.Second
	fractional := hilEvidence()
	fractional.Decision.ValidityWindow = 90*time.Second + 500*time.Millisecond
	pastDeadline := hilEvidence()
	pastDeadline.Decision.ValidityWindow = 601 * time.Second
	underWindow := hilEvidence()
	underWindow.Decision.SafetyMaximum = 89 * time.Second
	pastHour := hilEvidence()
	pastHour.Decision.SafetyMaximum = time.Hour + time.Second

	for name, evidence := range map[string]*store.HILTimingEvidence{
		"no validity window":                noWindow,
		"a negative window":                 negative,
		"a fractional window":               fractional,
		"a window past the task deadline":   pastDeadline,
		"a safety maximum under the window": underWindow,
		"a safety maximum past an hour":     pastHour,
	} {
		if validHILTimingAssignment(evidence, definition, 600) {
			t.Fatalf("%s was accepted", name)
		}
	}

	exact := hilEvidence()
	exact.Decision.ValidityWindow = 600 * time.Second
	exact.Decision.SafetyMaximum = time.Hour
	if !validHILTimingAssignment(exact, definition, 600) {
		t.Fatal("a window exactly at the deadline was refused")
	}
	if validHILTimingAssignment(hilEvidence(), definition, 0) {
		t.Fatal("a task with no deadline was accepted")
	}
}

// And the source has to agree with the sample count: a measured window
// needs measurements behind it, a default may not claim any, and a source
// nobody reviewed is refused outright.
func TestHILTimingSourceMustAgreeWithItsSamples(t *testing.T) {
	definition := hilDefinition()

	for name, answer := range map[string]struct {
		source  string
		samples int
		held    bool
	}{
		"a default with no samples":         {"default", 0, true},
		"a default with four samples":       {"default", 4, true},
		"a default claiming measurements":   {"default", 5, false},
		"a configured window":               {"hil.conf", 1, true},
		"a configured window over-claiming": {"hil.conf", 9, false},
		"an observed window":                {"observed", 5, true},
		"an observed window under-sampled":  {"observed", 4, false},
		"a capped observation":              {"observed-capped", 200, true},
		"a source nobody reviewed":          {"operator", 7, false},
		"no source at all":                  {"", 7, false},
	} {
		evidence := hilEvidence()
		evidence.Decision.Source = answer.source
		evidence.Decision.Samples = answer.samples
		if got := validHILTimingAssignment(evidence, definition, 600); got != answer.held {
			t.Fatalf("%s = %v", name, got)
		}
	}

	negativeSamples := hilEvidence()
	negativeSamples.Decision.Samples = -1
	tooManySamples := hilEvidence()
	tooManySamples.Decision.Samples = 10001
	negativeRejects := hilEvidence()
	negativeRejects.Decision.RejectedRows = -1
	tooManyRejects := hilEvidence()
	tooManyRejects.Decision.RejectedRows = 10001

	for name, evidence := range map[string]*store.HILTimingEvidence{
		"a negative sample count":    negativeSamples,
		"more samples than possible": tooManySamples,
		"a negative rejected count":  negativeRejects,
		"more rejects than possible": tooManyRejects,
	} {
		if validHILTimingAssignment(evidence, definition, 600) {
			t.Fatalf("%s was accepted", name)
		}
	}

	atBound := hilEvidence()
	atBound.Decision.Samples = 10000
	atBound.Decision.RejectedRows = 10000
	if !validHILTimingAssignment(atBound, definition, 600) {
		t.Fatal("counts exactly at the bound were refused")
	}
}
