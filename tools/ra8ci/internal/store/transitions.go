package store

import (
	"fmt"
	"sort"
)

// The run, task and attempt state machines were never stated anywhere. Each
// write site carried its own idea of what was legal as an `AND state='...'`
// clause: runs.go, attempts.go, dispatch.go, dispatch_reaper.go,
// board_hil_completion.go and cancellation.go between them hold every edge the
// control plane actually takes. This file states the three machines once, so a
// transition can be checked before the write and so the whole set is reviewable
// in one place instead of being reconstructed from SQL.
//
// The tables below are the edges the code takes today, not an aspiration. The
// state sets match the CHECK constraints in migrations/0001_initial.sql, and
// TestMachineStatesMatchSchema pins that.

// stateMachine is a directed graph over the states of one relation. A state
// with no outgoing edges is terminal.
type stateMachine struct {
	relation string
	edges    map[string][]string
}

// runMachine: runs are created queued, start when the first attempt is claimed,
// and end terminal. A run with no runnable work can go straight to terminal.
var runMachine = stateMachine{
	relation: "runs",
	edges: map[string][]string{
		"queued":   {"running", "terminal"},
		"running":  {"terminal"},
		"terminal": {},
	},
}

// taskMachine: a task is scheduled at creation, runs once claimed, and ends in
// one of the outcome states. Two edges lead backwards on purpose: the reaper
// requeues a lost attempt's task and a preempted HIL task is requeued, both
// running -> scheduled.
var taskMachine = stateMachine{
	relation: "tasks",
	edges: map[string][]string{
		"scheduled": {"running", "cancelled", "skipped"},
		"running":   {"succeeded", "failed", "timed_out", "cancelled", "preempted", "lost", "scheduled"},
		"succeeded": {},
		"failed":    {},
		"timed_out": {},
		"cancelled": {},
		"preempted": {},
		"lost":      {},
		"skipped":   {},
	},
}

// attemptMachine: a locally claimed attempt is inserted running; an agent
// assignment is inserted issued and becomes running when the agent
// acknowledges. "acknowledged" is legal in the schema and reachable here, but
// no call site writes it today: ClaimAgentTask goes issued -> running in one
// step. It stays in the machine so the reaper's issued/acknowledged/running
// candidate set keeps its meaning.
var attemptMachine = stateMachine{
	relation: "task_attempts",
	edges: map[string][]string{
		"issued":       {"acknowledged", "running", "cancelled", "lost"},
		"acknowledged": {"running", "cancelled", "lost"},
		"running":      {"succeeded", "failed", "timed_out", "cancelled", "preempted", "lost"},
		"succeeded":    {},
		"failed":       {},
		"timed_out":    {},
		"cancelled":    {},
		"preempted":    {},
		"lost":         {},
	},
}

// known reports whether the state exists in this machine at all. An unknown
// state is a bug or a schema change that never reached this file.
func (m stateMachine) known(state string) bool {
	_, exists := m.edges[state]
	return exists
}

// terminal reports whether the state has no outgoing edges.
func (m stateMachine) terminal(state string) bool {
	return len(m.edges[state]) == 0 && m.known(state)
}

// allows reports whether from -> to is an edge of this machine.
func (m stateMachine) allows(from, to string) bool {
	for _, next := range m.edges[from] {
		if next == to {
			return true
		}
	}
	return false
}

// states returns every state of the machine, sorted, for tests and messages.
func (m stateMachine) states() []string {
	all := make([]string, 0, len(m.edges))
	for state := range m.edges {
		all = append(all, state)
	}
	sort.Strings(all)
	return all
}

// check returns nil when the transition is legal, and an ErrConflict-wrapped
// error naming the relation and both states otherwise. A transition out of a
// terminal state says so, because that is the interesting case in a race: two
// writers finishing the same row.
func (m stateMachine) check(from, to string) error {
	if !m.known(from) {
		return fmt.Errorf("%w: %s has no state %q", ErrInvalid, m.relation, from)
	}
	if !m.known(to) {
		return fmt.Errorf("%w: %s has no state %q", ErrInvalid, m.relation, to)
	}
	if m.allows(from, to) {
		return nil
	}
	if m.terminal(from) {
		return fmt.Errorf("%w: %s is already %s", ErrConflict, m.relation, from)
	}
	return fmt.Errorf("%w: %s cannot go %s -> %s", ErrConflict, m.relation, from, to)
}

// CheckRunTransition reports whether a run may move from one state to the next.
func CheckRunTransition(from, to string) error { return runMachine.check(from, to) }

// CheckTaskTransition reports whether a task may move from one state to the next.
func CheckTaskTransition(from, to string) error { return taskMachine.check(from, to) }

// CheckAttemptTransition reports whether an attempt may move from one state to
// the next.
func CheckAttemptTransition(from, to string) error { return attemptMachine.check(from, to) }

// TerminalTaskState reports whether a task state is an outcome state. The
// ended_at CHECK constraint in 0001_initial.sql is written over exactly this
// set.
func TerminalTaskState(state string) bool { return taskMachine.terminal(state) }

// TerminalRunState reports whether a run has already been closed.
func TerminalRunState(state string) bool { return runMachine.terminal(state) }

// KnownRunState reports whether a run state exists in the machine at all.
//
// The other predicates here answer questions about a state the caller already
// believes in. A reader outside the plane has the earlier question: the run
// API hands a submitter a state string, and telling one the plane can be in
// from one no run ever carries needs the vocabulary, which is stated here and
// nowhere else.
func KnownRunState(state string) bool { return runMachine.known(state) }

// RunStartable reports whether a run in this state still has to be started.
// The agent dispatch candidate query takes queued and running runs alike, so
// the write site needs the machine to say which of the two it is looking at.
func RunStartable(state string) bool { return runMachine.allows(state, "running") }

// AttemptReapable reports whether an attempt in this state is still a
// candidate for fencing. The reaper's candidate set is not a list it keeps of
// its own: it is exactly the states from which the machine allows an attempt
// to be lost.
func AttemptReapable(state string) bool { return attemptMachine.allows(state, "lost") }
