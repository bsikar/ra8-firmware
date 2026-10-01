// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// A commit whose runs carry no workflow name is not the same as a commit with
// no runs, and neither is the same as a commit carrying other workflows. The
// refusal has to tell those three apart, because they send an operator to
// three different places: the listing is empty, the forge answered runs it
// would not name, or the workflow asked for is simply not one of them.
func TestRunsCarryingNoWorkflowNameAreSaidToBeNameless(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(80, "", "completed", "success"),
		listedEvidenceRun(81, "", "completed", "failure"),
	)

	_, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if !errors.Is(err, ErrEvidenceRunNotFound) {
		t.Fatalf("%v, want ErrEvidenceRunNotFound", err)
	}
	message := err.Error()
	if !strings.Contains(message, "runs of no named workflow") {
		t.Fatalf("refusal does not say the runs are nameless: %s", message)
	}
	if strings.Contains(message, "no workflow runs") {
		t.Fatalf("nameless runs were reported as no runs at all: %s", message)
	}
	if !strings.Contains(message, evidenceRunHead) {
		t.Fatalf("refusal does not name the commit: %s", message)
	}
}

// One named workflow beside nameless runs is named on its own: the nameless
// ones are passed over rather than reported as an empty pair of quotes.
func TestANamelessRunIsPassedOverWhenAnotherWorkflowIsNamed(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(82, "", "completed", "success"),
		listedEvidenceRun(83, "Docs", "completed", "success"),
		listedEvidenceRun(84, "", "completed", "success"),
	)

	_, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if !errors.Is(err, ErrEvidenceRunNotFound) {
		t.Fatalf("%v, want ErrEvidenceRunNotFound", err)
	}
	message := err.Error()
	if !strings.Contains(message, `"Docs"`) {
		t.Fatalf("refusal does not name the workflow the commit carries: %s", message)
	}
	if strings.Contains(message, `""`) {
		t.Fatalf("refusal names a nameless run as an empty workflow: %s", message)
	}
	if strings.Contains(message, "runs of no named workflow") {
		t.Fatalf("a commit carrying a named workflow was called nameless: %s", message)
	}
}

// The workflows are named in listing order, which is the order the forge
// answered in, so two reads of one commit describe it the same way.
func TestTheListedWorkflowsAreNamedInTheOrderTheyWereListed(t *testing.T) {
	listing := evidenceRunListing(
		listedEvidenceRun(85, "Release", "completed", "success"),
		listedEvidenceRun(86, "Docs", "completed", "success"),
		listedEvidenceRun(87, "Release", "completed", "failure"),
		listedEvidenceRun(88, "Audit", "completed", "success"),
	)

	_, err := SelectEvidenceRun(listing, evidenceRunWorkflow)
	if !errors.Is(err, ErrEvidenceRunNotFound) {
		t.Fatalf("%v, want ErrEvidenceRunNotFound", err)
	}
	message := err.Error()
	release, docs, audit := strings.Index(message, `"Release"`), strings.Index(message, `"Docs"`), strings.Index(message, `"Audit"`)
	if release < 0 || docs < 0 || audit < 0 {
		t.Fatalf("refusal does not name every workflow listed: %s", message)
	}
	if !(release < docs && docs < audit) {
		t.Fatalf("the workflows are not named in listing order: %s", message)
	}
	if strings.Count(message, `"Release"`) != 1 {
		t.Fatalf("a repeated workflow is named twice: %s", message)
	}
}
