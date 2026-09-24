package store

import (
	"errors"
	"testing"
)

// The agent dispatch, acknowledgment and terminal-receipt paths all write run,
// task and attempt state. None of those writes can be exercised without a
// database, but the edges they take are decided in Go before the write, so
// they are pinned here: if a call site ever takes an edge the machine does not
// carry, one of these fails without needing Postgres.

// ClaimAgentTask claims a scheduled task and starts a queued run.
func TestAgentClaimEdgesAreLegal(t *testing.T) {
	if err := CheckTaskTransition("scheduled", "running"); err != nil {
		t.Fatalf("claiming a scheduled task: %v", err)
	}
	for _, state := range taskMachine.states() {
		if state == "scheduled" {
			continue
		}
		if err := CheckTaskTransition(state, "running"); err == nil {
			t.Fatalf("a %s task must not be claimable", state)
		}
	}
}

// RunStartable is the whole of the dispatch rule "start the run if it has not
// started": the candidate query takes queued and running runs alike.
func TestRunStartableIsTheQueuedEdge(t *testing.T) {
	for _, state := range runMachine.states() {
		want := state == "queued"
		if got := RunStartable(state); got != want {
			t.Fatalf("RunStartable(%q) = %v, want %v", state, got, want)
		}
		if want {
			if err := CheckRunTransition(state, "running"); err != nil {
				t.Fatalf("a startable run must have the edge: %v", err)
			}
		}
	}
	if RunStartable("running") {
		t.Fatal("a running run must not be started a second time")
	}
}

// AcknowledgeAgentAssignment only ever runs against an issued attempt: it
// either cancels it, when the run was cancelled before the agent answered, or
// moves it to running.
func TestAcknowledgmentEdgesAreLegal(t *testing.T) {
	for _, to := range []string{"running", "cancelled"} {
		if err := CheckAttemptTransition("issued", to); err != nil {
			t.Fatalf("issued -> %s: %v", to, err)
		}
	}
	// The task cancelled alongside an unacknowledged attempt is running, and
	// a task a sibling already ended is skipped rather than written over.
	if err := CheckTaskTransition("running", "cancelled"); err != nil {
		t.Fatalf("running -> cancelled: %v", err)
	}
	for _, ended := range []string{"succeeded", "failed", "timed_out", "cancelled", "lost"} {
		if err := CheckTaskTransition(ended, "cancelled"); err == nil {
			t.Fatalf("a %s task must not be cancelled again", ended)
		}
	}
}

// CompleteAgentAttempt writes one result to both the attempt and its task,
// after taskResultFor has downgraded a success whose evidence is incomplete.
// Every outcome protocol.TerminalReceipt allows has to land on a legal edge
// from running, for both relations.
func TestTerminalReceiptOutcomesAreLegalEdges(t *testing.T) {
	for _, outcome := range []string{"succeeded", "failed", "timed_out", "cancelled"} {
		for _, evidence := range []bool{true, false} {
			result := taskResultFor(outcome, evidence)
			if err := CheckAttemptTransition("running", result); err != nil {
				t.Fatalf("attempt running -> %s (outcome %s, evidence %v): %v",
					result, outcome, evidence, err)
			}
			if err := CheckTaskTransition("running", result); err != nil {
				t.Fatalf("task running -> %s (outcome %s, evidence %v): %v",
					result, outcome, evidence, err)
			}
		}
	}
	if got := taskResultFor("succeeded", false); got != "failed" {
		t.Fatalf("an unverifiable green must not be recorded as a success, got %q", got)
	}
	if got := taskResultFor("succeeded", true); got != "succeeded" {
		t.Fatalf("a verified success must stay succeeded, got %q", got)
	}
}

// A receipt for an attempt that is no longer running is refused before any
// write, and the refusal is a conflict rather than an invalid argument.
func TestTerminalReceiptRefusesAMovedAttempt(t *testing.T) {
	for _, state := range []string{"issued", "succeeded", "failed", "lost", "cancelled"} {
		err := CheckAttemptTransition(state, "succeeded")
		if err == nil {
			t.Fatalf("a %s attempt must not be finished", state)
		}
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("a %s attempt must conflict, got %v", state, err)
		}
	}
	if err := CheckAttemptTransition("running", "acknowledged"); err == nil {
		t.Fatal("a running attempt must not go back to acknowledged")
	}
}
