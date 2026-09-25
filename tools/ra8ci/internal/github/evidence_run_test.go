// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

const evidenceRunHead = "1234567890abcdef1234567890abcdef12345678"

const evidenceRunWorkflow = "Checks"

// listedEvidenceRun builds one run the way RunsOn reports it.
func listedEvidenceRun(id int64, workflow, status, conclusion string) CommitWorkflowRun {
	return CommitWorkflowRun{
		ID: id, Workflow: workflow, Attempt: 1,
		Event: "pull_request", Status: status, Conclusion: conclusion,
	}
}

// evidenceRunListing builds a listing about evidenceRunHead.
func evidenceRunListing(runs ...CommitWorkflowRun) CommitWorkflowRuns {
	return CommitWorkflowRuns{HeadSHA: evidenceRunHead, Runs: runs}
}

func TestTheSelectedRunIsTheDecidedRunOfTheNamedWorkflow(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(41, evidenceRunWorkflow, "completed", "failure"),
	)
	selected, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if err != nil {
		t.Fatalf("select the evidence run: %v", err)
	}
	if selected.ID != 41 {
		t.Fatalf("selected run %d, want 41", selected.ID)
	}
	if selected.Conclusion != "failure" {
		t.Fatalf("selected conclusion %q, want failure", selected.Conclusion)
	}
	// The whole point of selecting is to hand Outcomes a run it will grade.
	if !selected.Completed() {
		t.Fatal("the selected run is one Outcomes refuses")
	}
}

func TestSelectingLooksPastEveryOtherWorkflowOnTheCommit(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(70, "Docs", "completed", "success"),
		listedEvidenceRun(71, "Release", "completed", "failure"),
		listedEvidenceRun(72, evidenceRunWorkflow, "completed", "success"),
		listedEvidenceRun(73, "Labeller", "in_progress", ""),
	)
	selected, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if err != nil {
		t.Fatalf("select the evidence run: %v", err)
	}
	if selected.ID != 72 {
		t.Fatalf("selected run %d, want 72", selected.ID)
	}
}

func TestAWorkflowMustBeNamed(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(41, evidenceRunWorkflow, "completed", "success"),
	)
	for _, name := range []string{"", " ", "\t\n"} {
		if _, err := SelectEvidenceRun(listing, name); !errors.Is(err, ErrEvidenceWorkflowUnnamed) {
			t.Fatalf("workflow %q: %v, want ErrEvidenceWorkflowUnnamed", name, err)
		}
	}
}

func TestAWorkflowNameIsMatchedExactly(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(41, evidenceRunWorkflow, "completed", "success"),
	)
	for _, name := range []string{"checks", "CHECKS", " Checks", "Checks ", "Check"} {
		if _, err := SelectEvidenceRun(listing, name); !errors.Is(err, ErrEvidenceRunNotFound) {
			t.Fatalf("workflow %q: %v, want ErrEvidenceRunNotFound", name, err)
		}
	}
}

func TestANotFoundRefusalNamesWhatTheCommitCarries(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(70, "Docs", "completed", "success"),
		listedEvidenceRun(71, "Docs", "completed", "failure"),
		listedEvidenceRun(72, "Release", "completed", "success"),
	)
	_, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if !errors.Is(err, ErrEvidenceRunNotFound) {
		t.Fatalf("%v, want ErrEvidenceRunNotFound", err)
	}
	message := err.Error()
	if !strings.Contains(message, `"Docs"`) || !strings.Contains(message, `"Release"`) {
		t.Fatalf("refusal does not name the listed workflows: %s", message)
	}
	// Named once each, however many runs carried the name.
	if strings.Count(message, `"Docs"`) != 1 {
		t.Fatalf("refusal names a workflow twice: %s", message)
	}
	if !strings.Contains(message, evidenceRunHead) {
		t.Fatalf("refusal does not name the commit: %s", message)
	}
}

func TestACommitWithNoRunsAtAllSaysSo(t *testing.T) {
	_, err := SelectEvidenceRun(evidenceRunListing(), evidenceRunWorkflow)
	if !errors.Is(err, ErrEvidenceRunNotFound) {
		t.Fatalf("%v, want ErrEvidenceRunNotFound", err)
	}
	if !strings.Contains(err.Error(), "no workflow runs") {
		t.Fatalf("refusal does not say the commit carries nothing: %s", err)
	}
}

func TestARunStillExecutingRefusesTheSelection(t *testing.T) {
	for _, status := range []string{"queued", "in_progress", "waiting", "requested", "pending"} {
		listing := evidenceRunListing(
			listedEvidenceRun(41, evidenceRunWorkflow, status, ""),
		)
		if _, err := SelectEvidenceRun(listing, evidenceRunWorkflow); !errors.Is(err, ErrEvidenceRunIncomplete) {
			t.Fatalf("status %q: %v, want ErrEvidenceRunIncomplete", status, err)
		}
	}
}

func TestARerunInFlightRefusesEvenBesideADecidedRun(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(42, evidenceRunWorkflow, "in_progress", ""),
		listedEvidenceRun(41, evidenceRunWorkflow, "completed", "success"),
	)
	_, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if !errors.Is(err, ErrEvidenceRunIncomplete) {
		t.Fatalf("%v, want ErrEvidenceRunIncomplete", err)
	}
	if !strings.Contains(err.Error(), "42") {
		t.Fatalf("refusal does not name the unfinished run: %s", err)
	}
}

func TestARunThatDecidedNothingIsNotEvidence(t *testing.T) {
	for _, conclusion := range []string{"cancelled", "skipped", "stale", "action_required", ""} {
		listing := evidenceRunListing(
			listedEvidenceRun(41, evidenceRunWorkflow, "completed", conclusion),
		)
		if _, err := SelectEvidenceRun(listing, evidenceRunWorkflow); !errors.Is(err, ErrEvidenceRunUndecided) {
			t.Fatalf("conclusion %q: %v, want ErrEvidenceRunUndecided", conclusion, err)
		}
	}
}

func TestEveryAnswerAConclusionCanGiveIsSelectable(t *testing.T) {
	for _, conclusion := range []string{"success", "failure", "neutral", "timed_out"} {
		listing := evidenceRunListing(
			listedEvidenceRun(41, evidenceRunWorkflow, "completed", conclusion),
		)
		selected, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
		if err != nil {
			t.Fatalf("conclusion %q: %v", conclusion, err)
		}
		if selected.Conclusion != conclusion {
			t.Fatalf("selected conclusion %q, want %q", selected.Conclusion, conclusion)
		}
	}
}

func TestADecidedRunIsSelectedPastTheUndecidedOnes(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(43, evidenceRunWorkflow, "completed", "cancelled"),
		listedEvidenceRun(42, evidenceRunWorkflow, "completed", "skipped"),
		listedEvidenceRun(41, evidenceRunWorkflow, "completed", "failure"),
	)
	selected, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if err != nil {
		t.Fatalf("select the evidence run: %v", err)
	}
	if selected.ID != 41 {
		t.Fatalf("selected run %d, want 41", selected.ID)
	}
}

func TestTwoDecidedRunsAreRefusedRatherThanResolved(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(42, evidenceRunWorkflow, "completed", "success"),
		listedEvidenceRun(41, evidenceRunWorkflow, "completed", "failure"),
	)
	_, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if !errors.Is(err, ErrEvidenceRunAmbiguous) {
		t.Fatalf("%v, want ErrEvidenceRunAmbiguous", err)
	}
	if !strings.Contains(err.Error(), "41") || !strings.Contains(err.Error(), "42") {
		t.Fatalf("refusal does not name both runs: %s", err)
	}
}

func TestTwoDecidedRunsAgreeingIsStillAmbiguous(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(42, evidenceRunWorkflow, "completed", "success"),
		listedEvidenceRun(41, evidenceRunWorkflow, "completed", "success"),
	)
	if _, err := SelectEvidenceRun(listing, evidenceRunWorkflow); !errors.Is(err, ErrEvidenceRunAmbiguous) {
		t.Fatalf("%v, want ErrEvidenceRunAmbiguous", err)
	}
}

func TestAListingAboutNoCommitIsRefused(t *testing.T) {
	for _, head := range []string{"", "1234567", "refs/heads/main", evidenceRunHead + "0", "zzzz567890abcdef1234567890abcdef12345678"} {
		listing := CommitWorkflowRuns{
			HeadSHA: head,
			Runs:    []CommitWorkflowRun{listedEvidenceRun(41, evidenceRunWorkflow, "completed", "success")},
		}
		if _, err := SelectEvidenceRun(listing, evidenceRunWorkflow); !errors.Is(err, ErrInvalidCheckRunSHA) {
			t.Fatalf("head %q: %v, want ErrInvalidCheckRunSHA", head, err)
		}
	}
	// The zero value is what a listing nobody filled in looks like.
	if _, err := SelectEvidenceRun(CommitWorkflowRuns{}, evidenceRunWorkflow); !errors.Is(err, ErrInvalidCheckRunSHA) {
		t.Fatalf("zero listing: %v, want ErrInvalidCheckRunSHA", err)
	}
}

func TestARefusedSelectionCarriesNothing(t *testing.T) {
	refused := []struct {
		name     string
		listing  CommitWorkflowRuns
		workflow string
	}{
		{"no commit", CommitWorkflowRuns{}, evidenceRunWorkflow},
		{"no workflow named", evidenceRunListing(listedEvidenceRun(41, evidenceRunWorkflow, "completed", "success")), ""},
		{"no run of it", evidenceRunListing(listedEvidenceRun(41, "Docs", "completed", "success")), evidenceRunWorkflow},
		{"still executing", evidenceRunListing(listedEvidenceRun(41, evidenceRunWorkflow, "queued", "")), evidenceRunWorkflow},
		{"decided nothing", evidenceRunListing(listedEvidenceRun(41, evidenceRunWorkflow, "completed", "cancelled")), evidenceRunWorkflow},
		{"two decided", evidenceRunListing(
			listedEvidenceRun(41, evidenceRunWorkflow, "completed", "success"),
			listedEvidenceRun(42, evidenceRunWorkflow, "completed", "success"),
		), evidenceRunWorkflow},
	}
	for _, refusal := range refused {
		t.Run(refusal.name, func(t *testing.T) {
			selected, err := SelectEvidenceRun(refusal.listing, refusal.workflow)
			if err == nil {
				t.Fatal("the selection was not refused")
			}
			if selected != (CommitWorkflowRun{}) {
				t.Fatalf("a refused selection carried run %+v", selected)
			}
		})
	}
}
