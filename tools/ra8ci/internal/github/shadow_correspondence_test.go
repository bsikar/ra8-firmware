// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

const (
	headA = "0123456789abcdef0123456789abcdef01234567"
	headB = "89abcdef0123456789abcdef0123456789abcdef"
)

func catalogNames(t *testing.T) []string {
	t.Helper()
	loaded, err := catalog.Load()
	if err != nil {
		t.Fatalf("catalog.Load: %v", err)
	}
	return loaded.Names()
}

func testCorrespondence(t *testing.T, pairs map[string]string) *ShadowCorrespondence {
	t.Helper()
	correspondence, err := NewShadowCorrespondence(pairs, catalogNames(t))
	if err != nil {
		t.Fatalf("NewShadowCorrespondence: %v", err)
	}
	return correspondence
}

// A correspondence is only evidence if it names tasks that exist. Every name
// used in these tests is checked against the embedded catalog by construction,
// so a catalog rename breaks the tests rather than the lab.
func TestCorrespondenceRefusesATaskTheCatalogDoesNotCarry(t *testing.T) {
	_, err := NewShadowCorrespondence(map[string]string{"format": "lint", "no-such-task": "other"}, catalogNames(t))
	if !errors.Is(err, ErrShadowCorrespondenceInvalid) {
		t.Fatalf("want ErrShadowCorrespondenceInvalid, got %v", err)
	}
	if !strings.Contains(err.Error(), "no-such-task") {
		t.Fatalf("error should name the task, got %v", err)
	}
}

func TestCorrespondenceRefusesAnUncheckableDeclaration(t *testing.T) {
	names := catalogNames(t)
	cases := []struct {
		name       string
		pairs      map[string]string
		knownTasks []string
	}{
		{"no pairs", map[string]string{}, names},
		{"nil pairs", nil, names},
		{"nothing to check against", map[string]string{"format": "lint"}, nil},
		{"task name outside the catalog rule", map[string]string{"Format": "lint"}, names},
		{"empty job", map[string]string{"format": ""}, names},
		{"job with surrounding space", map[string]string{"format": " lint"}, names},
		{"job with a control character", map[string]string{"format": "lint\tjob"}, names},
		{"job over the bound", map[string]string{"format": strings.Repeat("j", maxActionsJobName+1)}, names},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			if _, err := NewShadowCorrespondence(testCase.pairs, testCase.knownTasks); !errors.Is(err, ErrShadowCorrespondenceInvalid) {
				t.Fatalf("want ErrShadowCorrespondenceInvalid, got %v", err)
			}
		})
	}
}

// One job reports one conclusion. Two tasks under it means a failure cannot be
// attributed, and grading both against it would report a conflict for a task
// that may have passed.
func TestCorrespondenceRefusesOneJobCoveringTwoTasks(t *testing.T) {
	_, err := NewShadowCorrespondence(map[string]string{"format": "lint", "tidy": "lint"}, catalogNames(t))
	if !errors.Is(err, ErrShadowCorrespondenceAmbiguous) {
		t.Fatalf("want ErrShadowCorrespondenceAmbiguous, got %v", err)
	}
}

func TestCorrespondenceReportsWhatItCovers(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"tidy": "lint-tidy", "format": "lint-format"})
	job, covered := correspondence.Job("format")
	if !covered || job != "lint-format" {
		t.Fatalf("Job(format) = %q %v", job, covered)
	}
	if _, covered := correspondence.Job("misra"); covered {
		t.Fatal("misra is not in this correspondence")
	}
	tasks := correspondence.Tasks()
	if len(tasks) != 2 || tasks[0] != "format" || tasks[1] != "tidy" {
		t.Fatalf("Tasks() = %v, want [format tidy]", tasks)
	}
	tasks[0] = "mutated"
	if again := correspondence.Tasks(); again[0] != "format" {
		t.Fatal("Tasks() returned the internal slice")
	}
}

// The end the collector exists for: raw outcomes in, a graded report out.
func TestCollectProducesObservationsCompareShadowRunGrades(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{
		"format": "lint-format",
		"tidy":   "lint-tidy",
	})
	collection, err := correspondence.Collect(
		[]PlaneOutcome{
			{Task: "tidy", HeadSHA: headA, Observed: "failure"},
			{Task: "format", HeadSHA: headA, Observed: "success"},
		},
		[]ActionsOutcome{
			{Job: "lint-format", HeadSHA: headA, Conclusion: "success"},
			{Job: "lint-tidy", HeadSHA: headA, Conclusion: "timed_out"},
		},
	)
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if collection.HeadSHA != headA {
		t.Fatalf("HeadSHA = %q", collection.HeadSHA)
	}
	if len(collection.Observations) != 2 || collection.Observations[0].Task != "format" {
		t.Fatalf("observations = %+v, want format first", collection.Observations)
	}
	if len(collection.NotRun) != 0 {
		t.Fatalf("NotRun = %v", collection.NotRun)
	}
	report, err := CompareShadowRun(collection.Observations)
	if err != nil {
		t.Fatalf("CompareShadowRun: %v", err)
	}
	if report.Agreed != 1 || report.Divergent != 1 || report.Conflicting != 0 || report.Indeterminate != 0 {
		t.Fatalf("report = %+v", report)
	}
	if !report.Clean() {
		t.Fatal("a vocabulary difference is not a reason to hold the gate")
	}
}

// Evidence with nowhere to go is refused, not dropped.
func TestCollectRefusesATaskTheCorrespondenceDoesNotCover(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format"})
	_, err := correspondence.Collect(
		[]PlaneOutcome{
			{Task: "format", HeadSHA: headA, Observed: "success"},
			{Task: "misra", HeadSHA: headA, Observed: "failure"},
		},
		[]ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: "success"}},
	)
	if !errors.Is(err, ErrShadowTaskNotCorrespondent) {
		t.Fatalf("want ErrShadowTaskNotCorrespondent, got %v", err)
	}
	if !strings.Contains(err.Error(), "misra") {
		t.Fatalf("error should name the task, got %v", err)
	}
}

// A correspondence is a statement about tasks, not a description of the
// workflow, so the workflow's other jobs pass through without complaint.
func TestCollectIgnoresAnActionsJobOutsideTheCorrespondence(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format"})
	collection, err := correspondence.Collect(
		[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
		[]ActionsOutcome{
			{Job: "lint-format", HeadSHA: headA, Conclusion: "success"},
			{Job: "publish-docs", HeadSHA: headA, Conclusion: "failure"},
		},
	)
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(collection.Observations) != 1 || collection.Observations[0].ActionsConclusion != "success" {
		t.Fatalf("observations = %+v", collection.Observations)
	}
}

// A commit that did not exercise a covered task is a normal commit, not a gap.
func TestCollectCountsCoveredTasksThisCommitDidNotRun(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{
		"format": "lint-format",
		"tidy":   "lint-tidy",
		"misra":  "static-misra",
	})
	collection, err := correspondence.Collect(
		[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
		[]ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: "success"}},
	)
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(collection.NotRun) != 2 || collection.NotRun[0] != "misra" || collection.NotRun[1] != "tidy" {
		t.Fatalf("NotRun = %v, want [misra tidy]", collection.NotRun)
	}
	if len(collection.Observations) != 1 {
		t.Fatalf("a task that did not run is not paired: %+v", collection.Observations)
	}
}

// The case that decides whether a report can be clean by accident: the plane
// ran a task and the covering job never reported. The pairing is kept, graded
// indeterminate, and the report is not clean.
func TestCollectKeepsAPairingActionsNeverReported(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format"})
	for _, testCase := range []struct {
		name    string
		actions []ActionsOutcome
	}{
		{"job absent", nil},
		{"job not completed", []ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: ""}}},
		{"outcome lost", []ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: "stale"}}},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			collection, err := correspondence.Collect(
				[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
				testCase.actions,
			)
			if err != nil {
				t.Fatalf("Collect: %v", err)
			}
			if len(collection.Observations) != 1 {
				t.Fatalf("the pairing must survive: %+v", collection.Observations)
			}
			report, err := CompareShadowRun(collection.Observations)
			if err != nil {
				t.Fatalf("CompareShadowRun: %v", err)
			}
			if report.Indeterminate != 1 || report.Agreed != 0 {
				t.Fatalf("report = %+v", report)
			}
			if report.Clean() {
				t.Fatal("a comparison that was never made is not one that passed")
			}
		})
	}
}

func TestCollectRefusesAnUngradableSet(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format", "tidy": "lint-tidy"})
	cases := []struct {
		name    string
		plane   []PlaneOutcome
		actions []ActionsOutcome
		want    error
	}{
		{
			name:  "no plane outcomes",
			plane: nil,
			want:  ErrShadowObservationInvalid,
		},
		{
			name:  "task reported twice",
			plane: []PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}, {Task: "format", HeadSHA: headA, Observed: "failure"}},
			want:  ErrShadowSetAmbiguous,
		},
		{
			name:    "job reported twice",
			plane:   []PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
			actions: []ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: "success"}, {Job: "lint-format", HeadSHA: headA, Conclusion: "failure"}},
			want:    ErrShadowSetAmbiguous,
		},
		{
			name:  "two commits on the plane side",
			plane: []PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}, {Task: "tidy", HeadSHA: headB, Observed: "success"}},
			want:  ErrShadowHeadMismatch,
		},
		{
			name:    "the Actions side judged another commit",
			plane:   []PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
			actions: []ActionsOutcome{{Job: "lint-format", HeadSHA: headB, Conclusion: "success"}},
			want:    ErrShadowHeadMismatch,
		},
		{
			name:  "head SHA that is not a commit",
			plane: []PlaneOutcome{{Task: "format", HeadSHA: "abc", Observed: "success"}},
			want:  ErrShadowObservationInvalid,
		},
		{
			name:  "conclusion outside the vocabulary",
			plane: []PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "exploded"}},
			want:  ErrShadowObservationInvalid,
		},
		{
			name:    "Actions conclusion outside the vocabulary",
			plane:   []PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
			actions: []ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: "exploded"}},
			want:    ErrShadowObservationInvalid,
		},
		{
			name:    "unusable job name",
			plane:   []PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
			actions: []ActionsOutcome{{Job: "", HeadSHA: headA, Conclusion: "success"}},
			want:    ErrShadowObservationInvalid,
		},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			if _, err := correspondence.Collect(testCase.plane, testCase.actions); !errors.Is(err, testCase.want) {
				t.Fatalf("want %v, got %v", testCase.want, err)
			}
		})
	}
}

// The plane's own vocabulary is what Collect accepts, so every conclusion a
// task can end with round-trips rather than being refused at the seam.
func TestCollectAcceptsEveryConclusionATaskCanEndWith(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format"})
	for state := range observedConclusions {
		conclusion, err := ObservedConclusion(state)
		if err != nil {
			t.Fatalf("ObservedConclusion(%q): %v", state, err)
		}
		collection, err := correspondence.Collect(
			[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: conclusion}},
			[]ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: "success"}},
		)
		if err != nil {
			t.Fatalf("Collect for state %q (%q): %v", state, conclusion, err)
		}
		if collection.Observations[0].Observed != conclusion {
			t.Fatalf("observed conclusion changed: %+v", collection.Observations[0])
		}
	}
}

// The mirror of the pairing Actions never reported: this plane ran nothing and
// Actions concluded. It is named, it stays in NotRun, and it is not a pairing.
func TestCollectNamesATaskActionsJudgedThatThisPlaneDidNotRun(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{
		"format": "lint-format",
		"tidy":   "lint-tidy",
	})
	collection, err := correspondence.Collect(
		[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
		[]ActionsOutcome{
			{Job: "lint-format", HeadSHA: headA, Conclusion: "success"},
			{Job: "lint-tidy", HeadSHA: headA, Conclusion: "failure"},
		},
	)
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(collection.NotRunJudged) != 1 {
		t.Fatalf("NotRunJudged = %+v, want one entry", collection.NotRunJudged)
	}
	judged := collection.NotRunJudged[0]
	if judged.Task != "tidy" || judged.Job != "lint-tidy" || judged.Conclusion != "failure" {
		t.Fatalf("NotRunJudged[0] = %+v", judged)
	}
	if len(collection.NotRun) != 1 || collection.NotRun[0] != "tidy" {
		t.Fatalf("a judged task is still one this commit did not exercise: %v", collection.NotRun)
	}
	if len(collection.Observations) != 1 || collection.Observations[0].Task != "format" {
		t.Fatalf("a task with nothing observed is never paired: %+v", collection.Observations)
	}
	report, err := CompareShadowRun(collection.Observations)
	if err != nil {
		t.Fatalf("CompareShadowRun: %v", err)
	}
	if !report.Clean() || len(report.Comparisons) != 1 {
		t.Fatalf("an unpaired verdict grades nothing: %+v", report)
	}
}

// A task nobody judged is ordinary. Only a stated outcome is news, so an
// absent job, an unfinished one and an outcome Actions lost are all silent.
func TestANotRunTaskWithNoActionsVerdictIsNotJudged(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{
		"format": "lint-format",
		"tidy":   "lint-tidy",
	})
	for _, testCase := range []struct {
		name    string
		actions []ActionsOutcome
	}{
		{"job absent", nil},
		{"job not completed", []ActionsOutcome{{Job: "lint-tidy", HeadSHA: headA, Conclusion: ""}}},
		{"outcome lost", []ActionsOutcome{{Job: "lint-tidy", HeadSHA: headA, Conclusion: "stale"}}},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			collection, err := correspondence.Collect(
				[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
				testCase.actions,
			)
			if err != nil {
				t.Fatalf("Collect: %v", err)
			}
			if len(collection.NotRunJudged) != 0 {
				t.Fatalf("nothing was stated about tidy: %+v", collection.NotRunJudged)
			}
			if len(collection.NotRun) != 1 || collection.NotRun[0] != "tidy" {
				t.Fatalf("NotRun = %v, want [tidy]", collection.NotRun)
			}
		})
	}
}

// The conclusion travels verbatim. A job the workflow skipped and a job that
// failed are different news, and collapsing them into "judged" would leave the
// one worth acting on indistinguishable from the one that is routine.
func TestTheActionsConclusionForANotRunTaskIsCarriedVerbatim(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{
		"format": "lint-format",
		"tidy":   "lint-tidy",
	})
	for _, conclusion := range []string{"success", "failure", "cancelled", "skipped", "timed_out", "neutral", "action_required"} {
		t.Run(conclusion, func(t *testing.T) {
			collection, err := correspondence.Collect(
				[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
				[]ActionsOutcome{{Job: "lint-tidy", HeadSHA: headA, Conclusion: conclusion}},
			)
			if err != nil {
				t.Fatalf("Collect: %v", err)
			}
			if len(collection.NotRunJudged) != 1 || collection.NotRunJudged[0].Conclusion != conclusion {
				t.Fatalf("NotRunJudged = %+v, want one %q", collection.NotRunJudged, conclusion)
			}
		})
	}
}

// Named in task order, the order NotRun and Observations are already in, so
// the three lists can be read beside each other.
func TestTheJudgedTasksThatDidNotRunAreNamedInTaskOrder(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{
		"format": "lint-format",
		"tidy":   "lint-tidy",
		"misra":  "static-misra",
	})
	collection, err := correspondence.Collect(
		[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
		[]ActionsOutcome{
			{Job: "static-misra", HeadSHA: headA, Conclusion: "failure"},
			{Job: "lint-tidy", HeadSHA: headA, Conclusion: "success"},
		},
	)
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(collection.NotRunJudged) != 2 {
		t.Fatalf("NotRunJudged = %+v, want two entries", collection.NotRunJudged)
	}
	if collection.NotRunJudged[0].Task != "misra" || collection.NotRunJudged[1].Task != "tidy" {
		t.Fatalf("NotRunJudged order = %+v, want misra then tidy", collection.NotRunJudged)
	}
}

// An Actions job outside the correspondence stays ignored. This slice reports
// covered tasks, never the whole workflow.
func TestAJobOutsideTheCorrespondenceIsNeverNamedAsJudged(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format"})
	collection, err := correspondence.Collect(
		[]PlaneOutcome{{Task: "format", HeadSHA: headA, Observed: "success"}},
		[]ActionsOutcome{
			{Job: "lint-format", HeadSHA: headA, Conclusion: "success"},
			{Job: "docs", HeadSHA: headA, Conclusion: "failure"},
		},
	)
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(collection.NotRunJudged) != 0 || len(collection.NotRun) != 0 {
		t.Fatalf("collection = %+v", collection)
	}
}
