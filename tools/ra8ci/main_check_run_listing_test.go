package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// The listing is the half of a publish document that neither command read.
// These tests hold it to one openable, named entry per run, and pin what is
// deliberately left alone: its order.

// listedRun is one run on the commit as GitHub listed it, built by hand
// rather than from a plan, because that is what these refusals are about: a
// listing that describes the commit in a way no report can be written from.
func listedRun(id int64, name string) github.PublishedCheckRun {
	return github.PublishedCheckRun{
		ID:         id,
		Name:       name,
		Mode:       github.ModeShadow,
		Status:     "completed",
		Conclusion: "neutral",
		Title:      "ra8ci observed failed",
		Summary:    "ra8ci observed failed on this commit.",
	}
}

// onePlannedTask is one real reviewed task planned as a shadow run, the
// smallest plan either command will take.
func onePlannedTask(t *testing.T) []plannedCheckRun {
	t.Helper()
	return []plannedCheckRun{plannedRun(t, github.ModeShadow, firstCatalogTask(t), "failed")}
}

// bothCommandsRefuse holds the two entry points to one refusal. The check
// guards them in one place for the reason checkPlanIsOneSubject does: they
// are given the same listing and neither of them read it.
func bothCommandsRefuse(t *testing.T, published github.PublishedCheckRuns, want string) {
	t.Helper()
	plan := onePlannedTask(t)
	_, err := reconcileCheckRunPlan(plan, published)
	if err == nil || !strings.Contains(err.Error(), want) {
		t.Fatalf("reconcile error = %v, want it to name %q", err, want)
	}
	_, err = surveyCheckRunPlan(plan, published)
	if err == nil || !strings.Contains(err.Error(), want) {
		t.Fatalf("survey error = %v, want it to name %q", err, want)
	}
}

func TestARunListedWithNoIdentifierIsRefused(t *testing.T) {
	bothCommandsRefuse(t, listing(listedRun(0, "ra8ci (shadow): build")), "no identifier")
}

func TestARunListedUnderNoNameIsRefused(t *testing.T) {
	bothCommandsRefuse(t, listing(listedRun(77, "   ")), "run 77 under no name")
}

func TestOneRunListedTwiceIsRefused(t *testing.T) {
	published := listing(
		listedRun(91, "ra8ci (shadow): build"),
		listedRun(91, "ra8ci (shadow): lint"),
	)
	bothCommandsRefuse(t, published, "run 91 twice")
}

// The identifier is read before the name. A run carrying neither is refused
// for the number: the refusal itself, every line of the report and the
// survey's own claimed set all point at a run by it, so a run without one is
// not a run an operator can be sent to whatever it is called.
func TestTheIdentifierIsReadBeforeTheName(t *testing.T) {
	plan := onePlannedTask(t)
	_, err := reconcileCheckRunPlan(plan, listing(listedRun(0, "")))
	if err == nil || !strings.Contains(err.Error(), "no identifier") {
		t.Fatalf("error = %v, want the identifier refusal", err)
	}
	if strings.Contains(err.Error(), "under no name") {
		t.Fatalf("error = %v, want no name refusal in it", err)
	}
}

// Two runs under one name are the state the reconciliation exists to make
// visible: GitHub keeps each post as its own run, so a repeated write leaves
// two. They are different runs with different identifiers and the listing is
// right about them.
func TestTwoRunsUnderOneNameAreNotADuplicate(t *testing.T) {
	published := listing(
		listedRun(12, "ra8ci (shadow): build"),
		listedRun(13, "ra8ci (shadow): build"),
	)
	if err := checkListingNamesEachRunOnce(published); err != nil {
		t.Fatalf("two runs under one name were refused: %v", err)
	}
}

// A listing in another order is the same commit. PublishedRuns sorts by name
// and then by identifier so one commit read twice produces one document, but
// that is its promise about paging GitHub, and refusing a listing here for
// carrying the same runs in another order would put a second definition of
// that sort in the command.
func TestAListingInAnotherOrderIsStillRead(t *testing.T) {
	published := listing(
		listedRun(30, "ra8ci (shadow): lint"),
		listedRun(20, "ra8ci (shadow): build"),
	)
	if err := checkListingNamesEachRunOnce(published); err != nil {
		t.Fatalf("a listing in another order was refused: %v", err)
	}
}

// A commit carrying nothing yet is the ordinary first publish, and the
// listing that describes it is empty rather than wrong.
func TestAnEmptyListingIsNotRefused(t *testing.T) {
	if err := checkListingNamesEachRunOnce(listing()); err != nil {
		t.Fatalf("an empty listing was refused: %v", err)
	}
	if _, err := reconcileCheckRunPlan(onePlannedTask(t), listing()); err != nil {
		t.Fatalf("reconcile refused an empty listing: %v", err)
	}
}
