// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"testing"
)

// unplannedListing builds a listing of runs under this plane's names on one
// commit, each carrying the identifier this plane would post it with.
func unplannedListing(t *testing.T, names ...string) PublishedCheckRuns {
	t.Helper()
	listing := PublishedCheckRuns{HeadSHA: reconcilerHead}
	for index, name := range names {
		mode := ModeAuthoritative
		if ownerOfContext(name) == contextShadow {
			mode = ModeShadow
		}
		identifier, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: reconcilerHead})
		if err != nil {
			t.Fatalf("CheckRunExternalID(%q): %v", name, err)
		}
		listing.Runs = append(listing.Runs, PublishedCheckRun{
			ID: int64(index + 1), Name: name, Mode: mode,
			Status: "completed", Conclusion: "neutral", ExternalID: identifier,
		})
	}
	return listing
}

func unplannedNames(t *testing.T) (string, string) {
	t.Helper()
	first, second := reconcilerTaskNames(t)
	planned, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("planned name: %v", err)
	}
	retired, err := CheckRunName(ModeAuthoritative, second)
	if err != nil {
		t.Fatalf("retired name: %v", err)
	}
	return planned, retired
}

// The whole point of the file: a run under a name no plan claims is reported,
// and a run under a planned name is not.
func TestARunNoPlanClaimsIsReported(t *testing.T) {
	planned, retired := unplannedNames(t)
	listing := unplannedListing(t, planned, retired)
	unplanned, err := UnplannedRuns(listing, []string{planned})
	if err != nil {
		t.Fatalf("UnplannedRuns: %v", err)
	}
	if len(unplanned) != 1 || unplanned[0].Name != retired {
		t.Fatalf("unplanned = %+v, want only %q", unplanned, retired)
	}
}

// A commit every planned name accounts for answers with nothing, and answers
// with an empty slice rather than a nil one so a caller encoding the answer
// writes [] and not null.
func TestACommitThePlanAccountsForReportsNothing(t *testing.T) {
	planned, retired := unplannedNames(t)
	listing := unplannedListing(t, planned, retired)
	unplanned, err := UnplannedRuns(listing, []string{planned, retired})
	if err != nil {
		t.Fatalf("UnplannedRuns: %v", err)
	}
	if unplanned == nil || len(unplanned) != 0 {
		t.Fatalf("unplanned = %#v, want an empty answer", unplanned)
	}
}

// An empty plan is refused rather than answered. Reading it as "nothing is
// claimed" would report every run on the commit as stale, which is the
// loudest possible way to relay a caller that failed to build a plan.
func TestAnEmptyPlanIsRefusedNotAnsweredWithEverything(t *testing.T) {
	planned, retired := unplannedNames(t)
	listing := unplannedListing(t, planned, retired)
	for _, plan := range [][]string{nil, {}} {
		unplanned, err := UnplannedRuns(listing, plan)
		if !errors.Is(err, ErrUnplannedPlanEmpty) {
			t.Fatalf("plan %#v: err = %v, want ErrUnplannedPlanEmpty", plan, err)
		}
		if unplanned != nil {
			t.Fatalf("plan %#v: unplanned = %+v, want nothing", plan, unplanned)
		}
	}
}

// A blank planned name claims no run and matches none, so it is refused
// rather than quietly letting a plan under-claim.
func TestABlankPlannedNameIsRefused(t *testing.T) {
	planned, retired := unplannedNames(t)
	listing := unplannedListing(t, planned, retired)
	unplanned, err := UnplannedRuns(listing, []string{planned, ""})
	if !errors.Is(err, ErrUnplannedPlanName) {
		t.Fatalf("err = %v, want ErrUnplannedPlanName", err)
	}
	if unplanned != nil {
		t.Fatalf("unplanned = %+v, want nothing", unplanned)
	}
}

// Names are matched exactly. Branch protection requires a check run name
// literally, so a name differing in case or spacing is a different name, and
// matching loosely would account for a retired run or report a live one.
func TestAPlannedNameIsMatchedExactly(t *testing.T) {
	planned, _ := unplannedNames(t)
	listing := unplannedListing(t, planned)
	for _, near := range []string{
		planned + " ",
		" " + planned,
		planned + "-extra",
		"ra8ci/" + planned,
	} {
		unplanned, err := UnplannedRuns(listing, []string{near})
		if err != nil {
			t.Fatalf("UnplannedRuns(%q): %v", near, err)
		}
		if len(unplanned) != 1 || unplanned[0].Name != planned {
			t.Fatalf("plan %q accounted for %q", near, planned)
		}
	}
}

// Both namespaces are answered over together: a stale shadow run is exactly
// as invisible to a plan as a stale authoritative one.
func TestBothNamespacesAreAnsweredOver(t *testing.T) {
	first, second := reconcilerTaskNames(t)
	shadow, err := CheckRunName(ModeShadow, second)
	if err != nil {
		t.Fatalf("shadow name: %v", err)
	}
	authoritative, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("authoritative name: %v", err)
	}
	listing := unplannedListing(t, authoritative, shadow)
	unplanned, err := UnplannedRuns(listing, []string{"ra8ci / nothing-plans-this"})
	if err != nil {
		t.Fatalf("UnplannedRuns: %v", err)
	}
	if len(unplanned) != 2 {
		t.Fatalf("unplanned = %+v, want both runs", unplanned)
	}
	modes := map[CheckRunMode]bool{}
	for _, run := range unplanned {
		modes[run.Mode] = true
	}
	if !modes[ModeShadow] || !modes[ModeAuthoritative] {
		t.Fatalf("unplanned = %+v, want one run of each mode", unplanned)
	}
}

// A run that has not finished is reported like any other: whether a run is
// accounted for is a question about its name, and an unfinished run under a
// name nothing plans is the same unaccounted-for run when it concludes.
func TestAnUnfinishedRunIsReportedLikeAnyOther(t *testing.T) {
	planned, retired := unplannedNames(t)
	listing := unplannedListing(t, planned, retired)
	for index := range listing.Runs {
		if listing.Runs[index].Name == retired {
			listing.Runs[index].Status = "in_progress"
			listing.Runs[index].Conclusion = ""
		}
	}
	unplanned, err := UnplannedRuns(listing, []string{planned})
	if err != nil {
		t.Fatalf("UnplannedRuns: %v", err)
	}
	if len(unplanned) != 1 || unplanned[0].Status != "in_progress" || unplanned[0].Conclusion != "" {
		t.Fatalf("unplanned = %+v, want the unfinished run verbatim", unplanned)
	}
}

// The answer keeps the listing's order, which PublishedRuns sorts, so one
// commit read twice produces one document.
func TestTheAnswerKeepsTheListingsOrder(t *testing.T) {
	planned, retired := unplannedNames(t)
	listing := unplannedListing(t, retired, planned, retired)
	listing.Runs[2].ID = 99
	unplanned, err := UnplannedRuns(listing, []string{planned})
	if err != nil {
		t.Fatalf("UnplannedRuns: %v", err)
	}
	if len(unplanned) != 2 || unplanned[0].ID != 1 || unplanned[1].ID != 99 {
		t.Fatalf("unplanned = %+v, want the listing's own order", unplanned)
	}
}

// A run under a retired task name still carries the identifier this plane
// computes for that name and commit, so it answers as ours: a stale run of
// our own and a name collision are different pieces of work.
func TestOurOwnStaleRunIsRecognisedWithoutAnIntendedRun(t *testing.T) {
	_, retired := unplannedNames(t)
	listing := unplannedListing(t, retired)
	if !PublishedByThisPlane(listing.Runs[0], reconcilerHead) {
		t.Fatal("a run under a retired name of ours did not answer as ours")
	}
}

// A run carrying somebody else's identifier, or none at all, is not ours. A
// listing cannot tell a foreign run from one posted before the field existed,
// and both are states an operator has to see rather than have answered.
func TestAForeignOrUnidentifiedRunIsNotOurs(t *testing.T) {
	_, retired := unplannedNames(t)
	listing := unplannedListing(t, retired)
	for _, identifier := range []string{"", "ra8ci-1-00000000000000000000000000000000", "somebody-else"} {
		run := listing.Runs[0]
		run.ExternalID = identifier
		if PublishedByThisPlane(run, reconcilerHead) {
			t.Fatalf("identifier %q answered as ours", identifier)
		}
	}
}

// The identifier is derived from the name and the commit, so a run of ours
// listed against another commit is not ours on this one: that is the shape a
// re-used branch produces.
func TestOurRunOnAnotherCommitIsNotOursHere(t *testing.T) {
	_, retired := unplannedNames(t)
	listing := unplannedListing(t, retired)
	other := "89abcdef0123456789abcdef0123456789abcdef"
	if PublishedByThisPlane(listing.Runs[0], other) {
		t.Fatal("a run identified for one commit answered as ours on another")
	}
	if !PublishedByThisPlane(listing.Runs[0], upperCommit(reconcilerHead)) {
		t.Fatal("GitHub's casing of a commit was read as a different commit")
	}
}

// A run this plane could never have posted under a name it could never have
// built is not ours, and is refused before any digest is compared.
func TestAnUnbuildableRunIsNotOurs(t *testing.T) {
	run := PublishedCheckRun{ID: 1, Name: "", ExternalID: "ra8ci-1-00000000000000000000000000000000"}
	if PublishedByThisPlane(run, reconcilerHead) {
		t.Fatal("a nameless run answered as ours")
	}
	named := PublishedCheckRun{ID: 1, Name: "ra8ci / whatever", ExternalID: "ra8ci-1-00000000000000000000000000000000"}
	if PublishedByThisPlane(named, "not-a-commit") {
		t.Fatal("a run on an invalid commit answered as ours")
	}
}

func upperCommit(value string) string {
	upper := []byte(value)
	for index := range upper {
		if upper[index] >= 'a' && upper[index] <= 'f' {
			upper[index] -= 'a' - 'A'
		}
	}
	return string(upper)
}
