package store

import (
	"errors"
	"testing"
)

// The attempt write sites in attempts.go no longer carry their own idea of
// what is legal: StartAttempt claims a task through CheckTaskTransition,
// FinishAttempt closes an attempt and its task through CheckAttemptTransition
// and CheckTaskTransition, and closeRunIfTerminal closes the run through
// CheckRunTransition. These tests pin the edges those sites now depend on, so
// an edit to the machine that breaks one of them fails here rather than in a
// Postgres-only integration run.

// everyResult is every attempt outcome validAttemptResult accepts.
var everyResult = []string{"succeeded", "failed", "timed_out", "cancelled", "preempted", "lost"}

func TestAcceptedAttemptResultsAreLegalAttemptEdges(t *testing.T) {
	for _, result := range everyResult {
		if err := CheckAttemptTransition("running", result); err != nil {
			t.Errorf("running -> %s is a result FinishAttempt accepts but not an edge: %v", result, err)
		}
	}
	if err := CheckAttemptTransition("running", "acknowledged"); err == nil {
		t.Error("running -> acknowledged must not be an attempt edge")
	}
}

func TestValidAttemptResultAgreesWithTheAttemptMachine(t *testing.T) {
	exit := 0
	for _, state := range attemptMachine.states() {
		in := FinishAttemptInput{
			AttemptID:        "0192f3a4-7b5c-7d6e-9f01-23456789abcd",
			ActorID:          "actor",
			Result:           state,
			ChildExitCode:    &exit,
			EvidenceComplete: true,
			HitDeadline:      state == "timed_out",
		}
		accepted := validAttemptResult(in)
		legal := CheckAttemptTransition("running", state) == nil
		if accepted != legal {
			t.Errorf("result %q: validAttemptResult=%v but running -> %q legal=%v", state, accepted, state, legal)
		}
	}
}

func TestTaskResultForDowngradesEvidencelessSuccess(t *testing.T) {
	if got := taskResultFor("succeeded", false); got != "failed" {
		t.Errorf("a success with incomplete evidence must fail the task, got %q", got)
	}
	if got := taskResultFor("succeeded", true); got != "succeeded" {
		t.Errorf("a success with complete evidence must succeed, got %q", got)
	}
	for _, result := range everyResult {
		if result == "succeeded" {
			continue
		}
		if got := taskResultFor(result, true); got != result {
			t.Errorf("taskResultFor(%q, true) = %q, want it carried through", result, got)
		}
	}
}

func TestEveryTaskResultIsALegalTaskEdge(t *testing.T) {
	for _, result := range everyResult {
		for _, evidence := range []bool{true, false} {
			taskResult := taskResultFor(result, evidence)
			if err := CheckTaskTransition("running", taskResult); err != nil {
				t.Errorf("attempt %s (evidence=%v) finishes its task running -> %s, not an edge: %v",
					result, evidence, taskResult, err)
			}
		}
	}
}

func TestOnlyAScheduledTaskCanBeClaimed(t *testing.T) {
	for _, state := range taskMachine.states() {
		err := CheckTaskTransition(state, "running")
		if state == "scheduled" {
			if err != nil {
				t.Errorf("StartAttempt must be able to claim a scheduled task: %v", err)
			}
			continue
		}
		if err == nil {
			t.Errorf("a %s task must not be claimable", state)
		} else if !errors.Is(err, ErrConflict) {
			t.Errorf("claiming a %s task must conflict, got %v", state, err)
		}
	}
}

func TestOnlyAQueuedRunStartsOnFirstClaim(t *testing.T) {
	for _, state := range runMachine.states() {
		legal := CheckRunTransition(state, "running") == nil
		if legal != (state == "queued") {
			t.Errorf("run %s -> running legal=%v, StartAttempt starts a queued run and no other", state, legal)
		}
	}
}

func TestARunClosesFromQueuedOrRunningOnly(t *testing.T) {
	for _, state := range []string{"queued", "running"} {
		if err := CheckRunTransition(state, "terminal"); err != nil {
			t.Errorf("closeRunIfTerminal must close a %s run: %v", state, err)
		}
	}
	err := CheckRunTransition("terminal", "terminal")
	if err == nil {
		t.Fatal("closing an already terminal run must not be an edge")
	}
	if !errors.Is(err, ErrConflict) {
		t.Errorf("closing a terminal run must conflict, got %v", err)
	}
}

func TestSkippingADependentTaskIsAScheduledEdge(t *testing.T) {
	if err := CheckTaskTransition("scheduled", "skipped"); err != nil {
		t.Errorf("skipDescendants skips scheduled dependents: %v", err)
	}
	if err := CheckTaskTransition("running", "skipped"); err == nil {
		t.Error("a running task must not be skippable")
	}
}
