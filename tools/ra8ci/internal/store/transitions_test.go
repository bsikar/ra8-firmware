package store

import (
	"errors"
	"os"
	"regexp"
	"strings"
	"testing"
)

// The machines are only worth having if their state sets are the schema's. This
// reads the CHECK constraints out of the initial migration rather than
// restating them, so a schema change that adds a state fails here instead of at
// runtime.
func TestMachineStatesMatchSchema(t *testing.T) {
	sql, err := os.ReadFile("../../migrations/0001_initial.sql")
	if err != nil {
		t.Fatalf("read migration: %v", err)
	}
	for _, machine := range []stateMachine{runMachine, taskMachine, attemptMachine} {
		want := schemaStates(t, string(sql), machine.relation)
		got := strings.Join(machine.states(), ",")
		if got != want {
			t.Errorf("%s states = %s, schema says %s", machine.relation, got, want)
		}
	}
}

// schemaStates returns the sorted state list of the relation's state CHECK
// constraint, joined with commas.
func schemaStates(t *testing.T, sql, relation string) string {
	t.Helper()
	table := regexp.MustCompile(`(?s)CREATE TABLE ` + relation + ` \((.*?)\n\);`).FindStringSubmatch(sql)
	if table == nil {
		t.Fatalf("no CREATE TABLE %s in the initial migration", relation)
	}
	check := regexp.MustCompile(`\n\s+state text NOT NULL CHECK \(state IN \(([^)]*)\)\)`).FindStringSubmatch(table[1])
	if check == nil {
		t.Fatalf("no state CHECK constraint on %s", relation)
	}
	states := strings.Split(check[1], ",")
	for i, state := range states {
		states[i] = strings.Trim(strings.TrimSpace(state), "'")
	}
	sorted := append([]string(nil), states...)
	for i := range sorted {
		for j := i + 1; j < len(sorted); j++ {
			if sorted[j] < sorted[i] {
				sorted[i], sorted[j] = sorted[j], sorted[i]
			}
		}
	}
	return strings.Join(sorted, ",")
}

// Every edge the control plane actually writes today. Each case names the call
// site it came from, so a future edit that drops an edge has to argue with a
// named caller rather than with a table.
func TestLegalTransitionsMatchTheWriteSites(t *testing.T) {
	cases := []struct {
		site     string
		check    func(string, string) error
		from, to string
	}{
		{"attempts.go StartAttempt: first claim starts the run", CheckRunTransition, "queued", "running"},
		{"dispatch.go ClaimAgentTask: same, for an agent", CheckRunTransition, "queued", "running"},
		{"attempts.go closeRunIfTerminal", CheckRunTransition, "running", "terminal"},
		{"attempts.go StartAttempt: claim a scheduled task", CheckTaskTransition, "scheduled", "running"},
		{"cancellation.go: cancel scheduled work", CheckTaskTransition, "scheduled", "cancelled"},
		{"attempts.go skipDescendants: prerequisite_failed", CheckTaskTransition, "scheduled", "skipped"},
		{"attempts.go FinishAttempt", CheckTaskTransition, "running", "succeeded"},
		{"dispatch.go agent receipt", CheckTaskTransition, "running", "failed"},
		{"dispatch_reaper.go: retry after a lost attempt", CheckTaskTransition, "running", "scheduled"},
		{"board_hil_completion.go: requeue a preempted HIL task", CheckTaskTransition, "running", "scheduled"},
		{"dispatch_reaper.go: no retries left", CheckTaskTransition, "running", "lost"},
		{"dispatch.go AcknowledgeAssignment", CheckAttemptTransition, "issued", "running"},
		{"dispatch.go: run cancelled before ack", CheckAttemptTransition, "issued", "cancelled"},
		{"dispatch_reaper.go: fence expired", CheckAttemptTransition, "running", "lost"},
		{"board_hil_completion.go FinishHILAttempt", CheckAttemptTransition, "running", "succeeded"},
	}
	for _, testCase := range cases {
		if err := testCase.check(testCase.from, testCase.to); err != nil {
			t.Errorf("%s: %s -> %s rejected: %v", testCase.site, testCase.from, testCase.to, err)
		}
	}
}

func TestIllegalTransitionsConflict(t *testing.T) {
	cases := []struct {
		why      string
		check    func(string, string) error
		from, to string
	}{
		{"a terminal run cannot restart", CheckRunTransition, "terminal", "running"},
		{"a run cannot go back to queued", CheckRunTransition, "running", "queued"},
		{"a scheduled task has not run, so it cannot succeed", CheckTaskTransition, "scheduled", "succeeded"},
		{"a finished task cannot be claimed again", CheckTaskTransition, "succeeded", "running"},
		{"a cancelled task stays cancelled", CheckTaskTransition, "cancelled", "running"},
		{"an attempt cannot succeed before it is acknowledged", CheckAttemptTransition, "issued", "succeeded"},
		{"a lost attempt cannot finish afterwards", CheckAttemptTransition, "lost", "succeeded"},
	}
	for _, testCase := range cases {
		err := testCase.check(testCase.from, testCase.to)
		if !errors.Is(err, ErrConflict) {
			t.Errorf("%s: %s -> %s gave %v, want ErrConflict", testCase.why, testCase.from, testCase.to, err)
		}
	}
}

func TestUnknownStateIsInvalidNotConflict(t *testing.T) {
	for _, state := range []string{"", "RUNNING", "done", "queued "} {
		if err := CheckTaskTransition("running", state); !errors.Is(err, ErrInvalid) {
			t.Errorf("task running -> %q gave %v, want ErrInvalid", state, err)
		}
		if err := CheckTaskTransition(state, "running"); !errors.Is(err, ErrInvalid) {
			t.Errorf("task %q -> running gave %v, want ErrInvalid", state, err)
		}
	}
}

// A terminal state is exactly one with no outgoing edges, and every machine has
// at least one, otherwise a row could cycle forever.
func TestTerminalStatesAreSinks(t *testing.T) {
	for _, machine := range []stateMachine{runMachine, taskMachine, attemptMachine} {
		sinks := 0
		for _, state := range machine.states() {
			if !machine.terminal(state) {
				continue
			}
			sinks++
			for _, other := range machine.states() {
				if machine.allows(state, other) {
					t.Errorf("%s: %s is terminal but has an edge to %s", machine.relation, state, other)
				}
			}
		}
		if sinks == 0 {
			t.Errorf("%s: no terminal state", machine.relation)
		}
	}
}

// Every state must be reachable from the machine's start state, or it is dead
// weight that will drift.
func TestEveryStateIsReachable(t *testing.T) {
	starts := map[string]string{"runs": "queued", "tasks": "scheduled", "task_attempts": "issued"}
	for _, machine := range []stateMachine{runMachine, taskMachine, attemptMachine} {
		seen := map[string]bool{starts[machine.relation]: true}
		queue := []string{starts[machine.relation]}
		for len(queue) > 0 {
			state := queue[0]
			queue = queue[1:]
			for _, next := range machine.edges[state] {
				if !seen[next] {
					seen[next] = true
					queue = append(queue, next)
				}
			}
		}
		for _, state := range machine.states() {
			if !seen[state] {
				t.Errorf("%s: %s is unreachable from %s", machine.relation, state, starts[machine.relation])
			}
		}
	}
}

func TestKnownRunStateIsTheMachineVocabulary(t *testing.T) {
	for _, state := range runMachine.states() {
		if !KnownRunState(state) {
			t.Fatalf("KnownRunState(%q) = false for a state of the machine", state)
		}
	}
	for _, state := range []string{"", "scheduled", "succeeded", "issued", "QUEUED", "unknown"} {
		if KnownRunState(state) {
			t.Fatalf("KnownRunState(%q) = true; no run carries it", state)
		}
	}
}
