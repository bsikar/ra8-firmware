// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"testing"
)

const shadowHead = "0123456789abcdef0123456789abcdef01234567"

func observation(task, observed, actions string) ShadowObservation {
	return ShadowObservation{
		Task:              task,
		HeadSHA:           shadowHead,
		Observed:          observed,
		ActionsJob:        task + " (ubuntu-latest)",
		ActionsConclusion: actions,
	}
}

func TestAgreementIsReportedWhenBothSidesSayTheSameThing(t *testing.T) {
	report, err := CompareShadowRun([]ShadowObservation{
		observation("build-firmware", "success", "success"),
		observation("unit-tests", "failure", "failure"),
	})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if report.Agreed != 2 || report.Divergent != 0 || report.Conflicting != 0 || report.Indeterminate != 0 {
		t.Fatalf("unexpected counts: %+v", report)
	}
	if !report.Clean() {
		t.Fatal("two agreements should be a clean report")
	}
	if report.HeadSHA != shadowHead {
		t.Fatalf("head SHA %q", report.HeadSHA)
	}
}

// A vocabulary difference branch protection cannot see is a note, not a reason
// to hold a required check where it is.
func TestConclusionsThatDifferButGateAlikeAreDivergentNotConflicting(t *testing.T) {
	for _, pair := range []struct{ observed, actions string }{
		{"failure", "timed_out"},
		{"timed_out", "cancelled"},
		{"cancelled", "action_required"},
		{"success", "skipped"},
		{"skipped", "neutral"},
		{"neutral", "success"},
	} {
		report, err := CompareShadowRun([]ShadowObservation{observation("build-firmware", pair.observed, pair.actions)})
		if err != nil {
			t.Fatalf("%s vs %s: %v", pair.observed, pair.actions, err)
		}
		if report.Divergent != 1 || report.Conflicting != 0 {
			t.Fatalf("%s vs %s graded %s", pair.observed, pair.actions, report.Comparisons[0].Verdict)
		}
		if !report.Clean() {
			t.Fatalf("%s vs %s should leave a clean report", pair.observed, pair.actions)
		}
	}
}

// The pairings the decision actually turns on: one side would merge, the other
// would not.
func TestDisagreementAboutTheGateIsConflictingBothDirections(t *testing.T) {
	report, err := CompareShadowRun([]ShadowObservation{
		observation("plane-blocks", "failure", "success"),
		observation("actions-blocks", "success", "failure"),
	})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if report.Conflicting != 2 {
		t.Fatalf("expected two conflicts, got %+v", report)
	}
	if report.Clean() {
		t.Fatal("a conflict must not read as clean")
	}
	for _, comparison := range report.Comparisons {
		if comparison.PlaneBlocks == comparison.ActionsBlocks {
			t.Fatalf("%s: gating effects agree on a conflicting pairing", comparison.Task)
		}
	}
}

// "stale" is internal/demand's word for a job whose outcome was lost, and an
// empty conclusion is a job that has not finished. Neither is evidence about
// ra8ci, and neither may read as agreement.
func TestActionsWithoutAVerdictIsIndeterminateAndNeverClean(t *testing.T) {
	for _, actions := range []string{"", "stale"} {
		report, err := CompareShadowRun([]ShadowObservation{observation("build-firmware", "success", actions)})
		if err != nil {
			t.Fatalf("actions %q: %v", actions, err)
		}
		if report.Indeterminate != 1 || report.Agreed != 0 {
			t.Fatalf("actions %q graded %s", actions, report.Comparisons[0].Verdict)
		}
		if report.Clean() {
			t.Fatalf("actions %q should not leave a clean report", actions)
		}
		if report.Comparisons[0].PlaneBlocks || report.Comparisons[0].ActionsBlocks {
			t.Fatalf("actions %q: gating computed for a pairing with no verdict", actions)
		}
	}
}

// The zero value of the grade is the one that cannot be mistaken for a pass.
func TestTheZeroVerdictIsIndeterminate(t *testing.T) {
	var verdict ShadowVerdict
	if verdict != ShadowIndeterminate || verdict.String() != "indeterminate" {
		t.Fatalf("zero verdict is %s", verdict)
	}
}

func TestAmbiguousAndMixedSetsAreRefusedRatherThanResolved(t *testing.T) {
	twice := []ShadowObservation{
		observation("build-firmware", "success", "success"),
		observation("build-firmware", "failure", "failure"),
	}
	if _, err := CompareShadowRun(twice); !errors.Is(err, ErrShadowSetAmbiguous) {
		t.Fatalf("one task paired twice returned %v", err)
	}
	other := observation("unit-tests", "success", "success")
	other.HeadSHA = "fedcba9876543210fedcba9876543210fedcba98"
	mixed := []ShadowObservation{observation("build-firmware", "success", "success"), other}
	if _, err := CompareShadowRun(mixed); !errors.Is(err, ErrShadowHeadMismatch) {
		t.Fatalf("two commits in one set returned %v", err)
	}
	if _, err := CompareShadowRun(nil); !errors.Is(err, ErrShadowObservationInvalid) {
		t.Fatalf("empty set returned %v", err)
	}
}

func TestAPairingThatCannotBeGradedHonestlyIsRefused(t *testing.T) {
	valid := observation("build-firmware", "success", "success")
	for name, broken := range map[string]func(ShadowObservation) ShadowObservation{
		"unnamed task":        func(o ShadowObservation) ShadowObservation { o.Task = ""; return o },
		"task outside rule":   func(o ShadowObservation) ShadowObservation { o.Task = "Build_Firmware"; return o },
		"short sha":           func(o ShadowObservation) ShadowObservation { o.HeadSHA = "0123abc"; return o },
		"branch not a sha":    func(o ShadowObservation) ShadowObservation { o.HeadSHA = "ra8ci/dev"; return o },
		"no actions job":      func(o ShadowObservation) ShadowObservation { o.ActionsJob = ""; return o },
		"observed empty":      func(o ShadowObservation) ShadowObservation { o.Observed = ""; return o },
		"observed is a state": func(o ShadowObservation) ShadowObservation { o.Observed = "succeeded"; return o },
		"observed is stale":   func(o ShadowObservation) ShadowObservation { o.Observed = "stale"; return o },
		"actions invented":    func(o ShadowObservation) ShadowObservation { o.ActionsConclusion = "passed"; return o },
	} {
		if _, err := CompareShadowRun([]ShadowObservation{broken(valid)}); !errors.Is(err, ErrShadowObservationInvalid) {
			t.Fatalf("%s returned %v", name, err)
		}
	}
}

// Two passes over the same pull request have to be diffable, so the order is
// the task name and not the order the caller happened to collect them in.
func TestComparisonsAreOrderedByTask(t *testing.T) {
	report, err := CompareShadowRun([]ShadowObservation{
		observation("unit-tests", "success", "success"),
		observation("build-firmware", "success", "success"),
		observation("lint", "success", "success"),
	})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	want := []string{"build-firmware", "lint", "unit-tests"}
	for i, task := range want {
		if report.Comparisons[i].Task != task {
			t.Fatalf("position %d is %q, want %q", i, report.Comparisons[i].Task, task)
		}
	}
}

// The comparison grades the conclusion the plane OBSERVED. A shadow run posts
// neutral whatever the task did, so grading what was posted would report every
// shadow pairing as agreeing with a neutral Actions job and disagreeing with
// everything else.
func TestTheObservedConclusionIsWhatEveryShadowRunCarriesIntoTheComparison(t *testing.T) {
	run, err := NewTaskCheckRun(ModeShadow, "build-firmware", shadowHead, "failed")
	if err != nil {
		t.Fatalf("new run: %v", err)
	}
	if run.Conclusion != shadowConclusion {
		t.Fatalf("shadow run posted %q", run.Conclusion)
	}
	report, err := CompareShadowRun([]ShadowObservation{{
		Task: "build-firmware", HeadSHA: run.HeadSHA, Observed: run.Observed,
		ActionsJob: "build", ActionsConclusion: "success",
	}})
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if report.Conflicting != 1 {
		t.Fatalf("a failed task against a passing job graded %s", report.Comparisons[0].Verdict)
	}
	posted, err := CompareShadowRun([]ShadowObservation{{
		Task: "build-firmware", HeadSHA: run.HeadSHA, Observed: run.Conclusion,
		ActionsJob: "build", ActionsConclusion: "success",
	}})
	if err != nil {
		t.Fatalf("compare posted: %v", err)
	}
	if posted.Conflicting != 0 {
		t.Fatal("grading the posted conclusion should hide the disagreement, which is why it is not graded")
	}
}

// Every conclusion the task machine can produce has to be gradeable, so a state
// added to the machine fails here instead of being refused in a live
// comparison.
func TestEveryObservedConclusionCanBeCompared(t *testing.T) {
	for state, conclusion := range observedConclusions {
		report, err := CompareShadowRun([]ShadowObservation{observation("build-firmware", conclusion, conclusion)})
		if err != nil {
			t.Fatalf("state %q conclusion %q: %v", state, conclusion, err)
		}
		if report.Agreed != 1 {
			t.Fatalf("state %q compared against itself graded %s", state, report.Comparisons[0].Verdict)
		}
	}
}

// The gating rule is one rule. If blockingConclusion and TaskCheckRun.Blocking
// ever disagreed, a comparison would grade a conflict the publisher does not
// see, or miss one it does.
func TestGatingRuleIsTheSameOneTheRunItselfApplies(t *testing.T) {
	for state := range observedConclusions {
		run, err := NewTaskCheckRun(ModeAuthoritative, "build-firmware", shadowHead, state)
		if err != nil {
			t.Fatalf("state %q: %v", state, err)
		}
		if run.Blocking() != blockingConclusion(run.Conclusion) {
			t.Fatalf("state %q: run says blocking=%v, comparison says %v",
				state, run.Blocking(), blockingConclusion(run.Conclusion))
		}
	}
}
