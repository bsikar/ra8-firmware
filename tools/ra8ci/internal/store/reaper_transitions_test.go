package store

import (
	"errors"
	"testing"
)

// The reaper's candidate query selects attempts in issued, acknowledged or
// running. That set is not a list the reaper keeps of its own; it is the set
// of states the attempt machine allows to be lost. If the machine grows an
// edge into lost, the query has to grow with it, and this test is what says so.
func TestReaperCandidateSetIsTheMachine(t *testing.T) {
	queried := map[string]bool{"issued": true, "acknowledged": true, "running": true}
	for _, state := range attemptMachine.states() {
		if AttemptReapable(state) != queried[state] {
			t.Fatalf("attempt state %q: machine says reapable=%v, the candidate query says %v",
				state, AttemptReapable(state), queried[state])
		}
	}
}

// Both reaper outcomes are legal edges of a running task, and neither is
// reachable from a task that already stopped.
func TestReapedTaskStateIsALegalEdge(t *testing.T) {
	for _, retry := range []bool{true, false} {
		next := reapedTaskState(retry)
		if err := CheckTaskTransition("running", next); err != nil {
			t.Fatalf("retry=%v yields running -> %s, which the task machine rejects: %v", retry, next, err)
		}
		for _, state := range taskMachine.states() {
			if state == "running" || state == "scheduled" {
				continue
			}
			if err := CheckTaskTransition(state, next); err == nil {
				t.Fatalf("a %s task should not be reapable to %s", state, next)
			}
		}
	}
}

func TestReapedTaskStateNames(t *testing.T) {
	if got := reapedTaskState(true); got != "scheduled" {
		t.Fatalf("a retried task is requeued, got %q", got)
	}
	if got := reapedTaskState(false); got != "lost" {
		t.Fatalf("an exhausted task is lost, got %q", got)
	}
}

// A task that moved out from under the reaper between the candidate query and
// the lock is a conflict, not a silent no-op: the audit record the reaper
// writes claims the task went running -> lost or running -> scheduled.
func TestReaperRejectsATaskThatAlreadyStopped(t *testing.T) {
	err := CheckTaskTransition("succeeded", "lost")
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("finishing a succeeded task again should conflict, got %v", err)
	}
	if err := CheckAttemptTransition("succeeded", "lost"); !errors.Is(err, ErrConflict) {
		t.Fatalf("fencing a succeeded attempt should conflict, got %v", err)
	}
}

// Cancellation replays instead of writing when the run is already closed, and
// terminal is the run machine's only sink.
func TestTerminalRunState(t *testing.T) {
	if !TerminalRunState("terminal") {
		t.Fatal("terminal is the run machine's sink")
	}
	for _, state := range runMachine.states() {
		if state != "terminal" && TerminalRunState(state) {
			t.Fatalf("run state %q is not terminal", state)
		}
	}
}

// The bulk cancel in RequestRunCancellation writes exactly this edge.
func TestScheduledTasksAreCancellable(t *testing.T) {
	if err := CheckTaskTransition("scheduled", "cancelled"); err != nil {
		t.Fatalf("cancelling an unassigned task is legal: %v", err)
	}
	if err := CheckTaskTransition("cancelled", "cancelled"); !errors.Is(err, ErrConflict) {
		t.Fatalf("cancelling twice should conflict, got %v", err)
	}
}
