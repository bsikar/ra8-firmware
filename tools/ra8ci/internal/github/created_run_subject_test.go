// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"net/http"
	"testing"
)

// postedRun is the run these tests treat as the one Publish sent.
func postedRun(t *testing.T) TaskCheckRun {
	t.Helper()
	run, err := NewTaskCheckRun(ModeShadow, "build", testHeadSHA, "succeeded")
	if err != nil {
		t.Fatalf("build run: %v", err)
	}
	return run
}

// answering is the response GitHub would give for a run recorded exactly as
// it was posted, before a test spoils one field of it.
func answering(t *testing.T, run TaskCheckRun) (checkRunResponse, string) {
	t.Helper()
	externalID, err := CheckRunExternalID(run)
	if err != nil {
		t.Fatalf("derive identifier: %v", err)
	}
	return checkRunResponse{
		ID: 4242, Name: run.Name, HeadSHA: run.HeadSHA, Status: run.Status,
		Conclusion: run.Conclusion, ExternalID: externalID,
	}, externalID
}

// The run GitHub records is the run that was posted, or it is not a publish
// to report.
func TestACreatedRunRecordedAsPostedIsAccepted(t *testing.T) {
	run := postedRun(t)
	created, externalID := answering(t, run)
	if err := checkCreatedRunIsTheRunPosted(created, run, externalID); err != nil {
		t.Fatalf("refused the run as posted: %v", err)
	}
}

// A check run is identified by its name and its commit, and the commit is the
// half Publish cannot see for itself. A run recorded on another commit is not
// in the listing any later reconciliation reads, so it is republished every
// pass while it sits somewhere else.
func TestACreatedRunOnAnotherCommitIsRefused(t *testing.T) {
	run := postedRun(t)
	created, externalID := answering(t, run)
	created.HeadSHA = "fedcba9876543210fedcba9876543210fedcba98"
	err := checkCreatedRunIsTheRunPosted(created, run, externalID)
	if !errors.Is(err, ErrCheckRunRejected) {
		t.Fatalf("error %v, want a rejected check run", err)
	}
}

// An answer that places the run on no commit at all is refused for the same
// reason, not tolerated as silence: GitHub answers a create with the commit,
// and the comparison exists to learn where the run landed.
func TestACreatedRunNamingNoCommitIsRefused(t *testing.T) {
	run := postedRun(t)
	created, externalID := answering(t, run)
	created.HeadSHA = ""
	err := checkCreatedRunIsTheRunPosted(created, run, externalID)
	if !errors.Is(err, ErrCheckRunRejected) {
		t.Fatalf("error %v, want a rejected check run", err)
	}
}

// GitHub renders a SHA in either case and the two spellings are one commit,
// which is how every other comparison in this package reads them.
func TestACommitSpelledInAnotherCaseIsTheSameCommit(t *testing.T) {
	run := postedRun(t)
	created, externalID := answering(t, run)
	created.HeadSHA = "0123456789ABCDEF0123456789ABCDEF01234567"
	if err := checkCreatedRunIsTheRunPosted(created, run, externalID); err != nil {
		t.Fatalf("refused a commit spelled in upper case: %v", err)
	}
}

// A run recorded as still queued disagrees with what was posted, and reads
// downstream as a write still in flight: an operator is sent to wait for an
// answer GitHub has already given.
func TestACreatedRunInAnotherStateIsRefused(t *testing.T) {
	for _, state := range []string{"queued", "in_progress", ""} {
		run := postedRun(t)
		created, externalID := answering(t, run)
		created.Status = state
		err := checkCreatedRunIsTheRunPosted(created, run, externalID)
		if !errors.Is(err, ErrCheckRunRejected) {
			t.Fatalf("status %q: error %v, want a rejected check run", state, err)
		}
	}
}

// The name and the conclusion keep the refusals they already had.
func TestACreatedRunUnderAnotherNameOrConclusionIsRefused(t *testing.T) {
	for _, testCase := range []struct {
		name  string
		spoil func(*checkRunResponse)
	}{
		{name: "another name", spoil: func(c *checkRunResponse) { c.Name = "ra8ci / build" }},
		{name: "no name", spoil: func(c *checkRunResponse) { c.Name = "" }},
		{name: "another conclusion", spoil: func(c *checkRunResponse) { c.Conclusion = "success" }},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			run := postedRun(t)
			created, externalID := answering(t, run)
			testCase.spoil(&created)
			err := checkCreatedRunIsTheRunPosted(created, run, externalID)
			if !errors.Is(err, ErrCheckRunRejected) {
				t.Fatalf("error %v, want a rejected check run", err)
			}
		})
	}
}

// The identifier keeps the one rule that is different from the others: an
// echoed identifier naming another run is refused, an absent one is silence
// about a field the listing carries independently.
func TestTheEchoedIdentifierKeepsItsOwnRule(t *testing.T) {
	run := postedRun(t)
	created, externalID := answering(t, run)
	created.ExternalID = ""
	if err := checkCreatedRunIsTheRunPosted(created, run, externalID); err != nil {
		t.Fatalf("refused an answer carrying no identifier: %v", err)
	}
	created.ExternalID = "ra8ci-1-00000000000000000000000000000000"
	err := checkCreatedRunIsTheRunPosted(created, run, externalID)
	if !errors.Is(err, ErrCheckRunRejected) {
		t.Fatalf("error %v, want a rejected check run", err)
	}
}

// Through Publish itself: a response recording the run on another commit is
// not a published run, and no identifier is handed back for it.
func TestPublishRefusesARunRecordedOnAnotherCommit(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	run := postedRun(t)
	server.mu.Lock()
	server.echo = false
	server.response = checkRunResponse{
		ID: 77, Name: run.Name, HeadSHA: "fedcba9876543210fedcba9876543210fedcba98",
		Status: "completed", Conclusion: run.Conclusion,
	}
	server.mu.Unlock()
	id, err := publisher.Publish(context.Background(), run, "summary")
	if id != 0 || !errors.Is(err, ErrCheckRunRejected) {
		t.Fatalf("publish returned %d, %v", id, err)
	}
	if _, runs := server.requests(); runs != 1 {
		t.Fatalf("posted %d runs, want 1", runs)
	}
}

// And a response recording it as still queued.
func TestPublishRefusesARunRecordedAsUnfinished(t *testing.T) {
	publisher, server := newCheckRunPublisher(t, ModeShadow)
	run := postedRun(t)
	server.mu.Lock()
	server.status, server.echo = http.StatusCreated, false
	server.response = checkRunResponse{
		ID: 78, Name: run.Name, HeadSHA: testHeadSHA,
		Status: "queued", Conclusion: run.Conclusion,
	}
	server.mu.Unlock()
	id, err := publisher.Publish(context.Background(), run, "summary")
	if id != 0 || !errors.Is(err, ErrCheckRunRejected) {
		t.Fatalf("publish returned %d, %v", id, err)
	}
}
