// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import "testing"

// The arithmetic TaskEvidence documents is the arithmetic three checks on the
// evidence page are written to: checkShortfallAddsUp reads Graded against the
// shortfall, checkCountedVerdicts reads the four verdicts against Graded and
// Observed, and the ready line prints Graded beside Agreed and Divergent.
// Until this test the only statement of it was a doc comment, and the comment
// was wrong: it said Graded was Agreed + Divergent, leaving out the verdict
// that holds the required check. A reader who believed it would have "fixed"
// checkCountedVerdicts into refusing every task that ever conflicted.
//
// So the accumulator states it here, over a real accumulation rather than a
// hand-built TaskEvidence, and a future change to the walk in
// AccumulateShadowEvidence has to come past this before it reaches the page.
func TestGradedCountsEveryVerdictIncludingConflicts(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build"), agreeing("lint")),
		gradedReport(t, evidenceCommitB, diverging("build"), conflicting("lint")),
		gradedReport(t, evidenceCommitC, conflicting("build"), ungraded("lint")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	for _, name := range []string{"build", "lint"} {
		task := taskEvidence(t, evidence, name)
		if want := task.Agreed + task.Divergent + task.Conflicting; task.Graded != want {
			t.Fatalf("%q: Graded = %d, Agreed + Divergent + Conflicting = %d (%+v)",
				name, task.Graded, want, task)
		}
		if want := task.Graded + task.Indeterminate; task.Observed != want {
			t.Fatalf("%q: Observed = %d, Graded + Indeterminate = %d (%+v)",
				name, task.Observed, want, task)
		}
	}
}

// The conflicting task is the one the stale comment would have undercounted,
// so it is stated as a number rather than only as an identity: three pairings,
// all graded, one of them a conflict.
func TestATaskThatConflictedIsGradedOnEveryCommitItProducedAVerdictOn(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build")),
		gradedReport(t, evidenceCommitB, diverging("build")),
		gradedReport(t, evidenceCommitC, conflicting("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	task := taskEvidence(t, evidence, "build")
	if task.Graded != 3 || task.Observed != 3 || task.Conflicting != 1 {
		t.Fatalf("build: Graded = %d, Observed = %d, Conflicting = %d, want 3, 3, 1 (%+v)",
			task.Graded, task.Observed, task.Conflicting, task)
	}
}

// The other half of the sentence: an unjudged pairing is observed and not
// graded, which is what the insufficient line's "paired on N, M never judged"
// is printed from.
func TestAnUnjudgedPairingIsObservedAndNotGraded(t *testing.T) {
	evidence, err := AccumulateShadowEvidence([]ShadowReport{
		gradedReport(t, evidenceCommitA, agreeing("build")),
		gradedReport(t, evidenceCommitB, ungraded("build")),
	})
	if err != nil {
		t.Fatalf("AccumulateShadowEvidence: %v", err)
	}
	task := taskEvidence(t, evidence, "build")
	if task.Graded != 1 || task.Observed != 2 || task.Indeterminate != 1 {
		t.Fatalf("build: Graded = %d, Observed = %d, Indeterminate = %d, want 1, 2, 1 (%+v)",
			task.Graded, task.Observed, task.Indeterminate, task)
	}
}
