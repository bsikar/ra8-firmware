package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// A listed run's state is read the same way its identifier and its name are:
// the listing arrives from GitHub verbatim, and the publish decision is taken
// from the status without anything asking whether the listing stated one.
// These tests hold a listed run to a state and pin what is deliberately left
// alone: the conclusion.

// statelessRun is one run on the commit listed without a status, which is the
// shape ReconcilePublish reads as a write still in flight.
func statelessRun(id int64, name string) github.PublishedCheckRun {
	run := listedRun(id, name)
	run.Status = ""
	return run
}

func TestARunListedInNoStateIsRefused(t *testing.T) {
	bothCommandsRefuse(t, listing(statelessRun(44, "ra8ci (shadow): build")), "run 44")
}

// The refusal names the run and what it was listed under, because that is all
// an operator has to go on: the run is on the commit and the listing says
// nothing about what it is doing there.
func TestTheStateRefusalNamesTheRunAndItsName(t *testing.T) {
	err := checkListedRunsAreStated(listing(statelessRun(44, "ra8ci (shadow): build")))
	if err == nil {
		t.Fatal("a run listed in no state was not refused")
	}
	for _, want := range []string{"run 44", `"ra8ci (shadow): build"`, "in no state"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("error = %v, want it to name %q", err, want)
		}
	}
}

// A status of blanks is the same listing as an empty one. Nothing downstream
// trims it, and ReconcilePublish compares it to "completed" exactly, so a run
// listed with spaces for a state is in flight forever just as surely.
func TestAStatusOfBlanksIsRefusedToo(t *testing.T) {
	run := listedRun(51, "ra8ci (shadow): lint")
	run.Status = "   "
	bothCommandsRefuse(t, listing(run), "run 51")
}

// A run that is queued or in progress is the state this listing exists to
// report. It has a status and no conclusion yet, and both commands read it as
// a write still in flight, which is an answer a later read can change.
func TestAnUnfinishedRunIsNotRefused(t *testing.T) {
	run := listedRun(60, "ra8ci (shadow): build")
	run.Status = "in_progress"
	run.Conclusion = ""
	if err := checkListedRunsAreStated(listing(run)); err != nil {
		t.Fatalf("an unfinished run was refused: %v", err)
	}
	if _, err := reconcileCheckRunPlan(onePlannedTask(t), listing(run)); err != nil {
		t.Fatalf("reconcile refused an unfinished run: %v", err)
	}
}

// *** A BLANK CONCLUSION IS DELIBERATELY NOT REFUSED. PublishedCheckRun says
// where it declares the field that the empty conclusion of a run that has not
// finished is carried verbatim, and it is the ordinary shape of the state a
// caller came to look for. What a completed run with no conclusion means is
// sameCheckRun's question, which already compares the field; asking it here
// would refuse listings GitHub answers with every day. ***
func TestABlankConclusionIsLeftAlone(t *testing.T) {
	run := listedRun(70, "ra8ci (shadow): build")
	run.Conclusion = ""
	if err := checkListedRunsAreStated(listing(run)); err != nil {
		t.Fatalf("a run with no conclusion was refused: %v", err)
	}
}

// The state is read after the identifier and the name. A run carrying none of
// the three is refused for the number, because every refusal and every
// reported line sends an operator to a run by it.
func TestTheIdentifierIsReadBeforeTheState(t *testing.T) {
	run := statelessRun(0, "")
	_, err := reconcileCheckRunPlan(onePlannedTask(t), listing(run))
	if err == nil || !strings.Contains(err.Error(), "no identifier") {
		t.Fatalf("error = %v, want the identifier refusal", err)
	}
	if strings.Contains(err.Error(), "in no state") {
		t.Fatalf("error = %v, want no state refusal in it", err)
	}
}

// A commit carrying nothing yet states nothing, and the listing that
// describes it is empty rather than wrong.
func TestAnEmptyListingStatesNothingAndIsRead(t *testing.T) {
	if err := checkListedRunsAreStated(listing()); err != nil {
		t.Fatalf("an empty listing was refused: %v", err)
	}
}
