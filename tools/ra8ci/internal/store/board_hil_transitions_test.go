package store

import "testing"

// CompleteBoardHILAttempt finishes a HIL attempt and either ends its task or
// requeues it. taskResultForHIL decides which, so every result it can return
// has to be an edge the task machine carries out of running, including the
// backwards one the cooperative board yield depends on.
func TestHILTaskResultsAreLegalTaskEdges(t *testing.T) {
	results := []string{"succeeded", "failed", "timed_out", "cancelled", "preempted"}
	for _, result := range results {
		for _, runCancelled := range []bool{false, true} {
			in := BoardHILCompletion{Result: result, EvidenceComplete: true}
			taskResult := taskResultForHIL(in, runCancelled)
			if err := CheckTaskTransition("running", taskResult); err != nil {
				t.Fatalf("task running -> %s (HIL result %s, run cancelled %v): %v",
					taskResult, result, runCancelled, err)
			}
			if err := CheckAttemptTransition("running", result); err != nil {
				t.Fatalf("attempt running -> %s: %v", result, err)
			}
		}
	}
}

// The yield is the only requeue: a preempted attempt requeues its task unless
// the run itself is being cancelled, in which case the task is cancelled.
func TestHILPreemptionRequeuesUnlessTheRunIsCancelled(t *testing.T) {
	preempted := BoardHILCompletion{Result: "preempted", EvidenceComplete: true}
	if got := taskResultForHIL(preempted, false); got != "scheduled" {
		t.Fatalf("a preempted HIL task must requeue, got %q", got)
	}
	if got := taskResultForHIL(preempted, true); got != "cancelled" {
		t.Fatalf("a preempted HIL task on a cancelled run must cancel, got %q", got)
	}
	if err := CheckTaskTransition("running", "scheduled"); err != nil {
		t.Fatalf("the cooperative yield edge must exist: %v", err)
	}
	if err := CheckTaskTransition("scheduled", "scheduled"); err == nil {
		t.Fatal("an already scheduled task must not be requeued again")
	}
}

// Every state validHILTerminalState accepts as a prior attempt state, other
// than running itself, is terminal: those are the replay cases, where the
// completion is checked against what was already written instead of writing
// again.
func TestHILReplayStatesAreTerminalOrRunning(t *testing.T) {
	for _, state := range attemptMachine.states() {
		if !validHILTerminalState(state) {
			continue
		}
		if state == "running" {
			continue
		}
		if !attemptMachine.terminal(state) {
			t.Fatalf("HIL replay state %q is neither running nor terminal", state)
		}
	}
}
