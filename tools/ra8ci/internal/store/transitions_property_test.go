package store

import (
	"errors"
	"math/rand"
	"testing"
)

// The unit tests in transitions_test.go name specific edges. These check the
// properties that have to hold for every state and every pair of states, so a
// machine edited later cannot quietly acquire a self-edge, an unreachable
// state, a dead end that is not an outcome, or a check that answers something
// other than "legal", ErrConflict or ErrInvalid.

func allMachines() []stateMachine {
	return []stateMachine{runMachine, taskMachine, attemptMachine}
}

// initialState is where a row of each relation is inserted.
func initialState(m stateMachine) string {
	switch m.relation {
	case "runs":
		return "queued"
	case "tasks":
		return "scheduled"
	default:
		return "issued"
	}
}

// check answers exactly three ways, and never panics, for any pair of strings
// drawn from the machine's own states plus states that belong to no machine.
func TestCheckIsTotalOverEveryPair(t *testing.T) {
	strangers := []string{"", "RUNNING", "done", "acknowledged ", "queued\n"}
	for _, m := range allMachines() {
		states := m.states()
		for _, from := range states {
			for _, to := range states {
				err := m.check(from, to)
				switch {
				case err == nil:
					if !m.allows(from, to) {
						t.Fatalf("%s: %s -> %s accepted without an edge", m.relation, from, to)
					}
				case errors.Is(err, ErrConflict):
					if m.allows(from, to) {
						t.Fatalf("%s: %s -> %s refused despite an edge", m.relation, from, to)
					}
				default:
					t.Fatalf("%s: %s -> %s returned %v, want nil or a conflict", m.relation, from, to, err)
				}
			}
			for _, stranger := range strangers {
				if err := m.check(from, stranger); !errors.Is(err, ErrInvalid) {
					t.Fatalf("%s: %s -> %q returned %v, want invalid", m.relation, from, stranger, err)
				}
				if err := m.check(stranger, from); !errors.Is(err, ErrInvalid) {
					t.Fatalf("%s: %q -> %s returned %v, want invalid", m.relation, stranger, from, err)
				}
			}
		}
	}
}

// No relation may transition to itself. A write site that "moves" a row to the
// state it is already in is a no-op the audit trail would still record.
func TestNoMachineHasASelfEdge(t *testing.T) {
	for _, m := range allMachines() {
		for _, state := range m.states() {
			if m.allows(state, state) {
				t.Fatalf("%s: %s -> %s is a self-edge", m.relation, state, state)
			}
		}
	}
}

// Every state is reachable from the state a row is inserted in, and every
// state can still reach a terminal one. The first catches a state nothing can
// produce; the second catches a non-terminal dead end, which is a row that
// would sit in the queue forever.
func TestEveryStateIsReachableAndCanTerminate(t *testing.T) {
	for _, m := range allMachines() {
		start := initialState(m)
		if !m.known(start) {
			t.Fatalf("%s: initial state %q is not in the machine", m.relation, start)
		}
		reached := reachableFrom(m, start)
		for _, state := range m.states() {
			if !reached[state] {
				t.Fatalf("%s: %s is not reachable from %s", m.relation, state, start)
			}
			if !canReachTerminal(m, state) {
				t.Fatalf("%s: %s is a dead end that is not an outcome", m.relation, state)
			}
		}
	}
}

// A random legal walk stays legal: every step the machine accepts lands on a
// known state, and the walk either terminates or is still on one of the
// backwards edges the control plane takes on purpose (a reaper retry or a
// cooperative board yield requeueing a task).
func TestRandomLegalWalksStayLegal(t *testing.T) {
	random := rand.New(rand.NewSource(1471))
	for _, m := range allMachines() {
		for walk := 0; walk < 200; walk++ {
			state := initialState(m)
			for step := 0; step < 50; step++ {
				next := m.edges[state]
				if len(next) == 0 {
					if !m.terminal(state) {
						t.Fatalf("%s: walk stopped on non-terminal %s", m.relation, state)
					}
					break
				}
				to := next[random.Intn(len(next))]
				if err := m.check(state, to); err != nil {
					t.Fatalf("%s: walk took %s -> %s and the machine refused it: %v",
						m.relation, state, to, err)
				}
				if !m.known(to) {
					t.Fatalf("%s: walk reached unknown state %s", m.relation, to)
				}
				state = to
			}
		}
	}
}

// The only cycle in any machine is the deliberate requeue, running ->
// scheduled on tasks, which the reaper retry and the cooperative board yield
// both take. With that one edge removed every machine is acyclic: any other
// cycle would let a row loop forever without producing an outcome.
func TestTheRequeueIsTheOnlyCycle(t *testing.T) {
	for _, m := range allMachines() {
		if cycle := findCycle(withoutRequeue(m)); cycle != "" {
			t.Fatalf("%s: cycle through %s with the requeue edge removed", m.relation, cycle)
		}
	}
	// And the requeue really is a cycle, so the test above is not vacuous.
	if !reachableFrom(taskMachine, "scheduled")["running"] || !taskMachine.allows("running", "scheduled") {
		t.Fatal("the task requeue edge is gone; the acyclic check above no longer means anything")
	}
}

// withoutRequeue copies the machine's edges minus the backwards requeue.
func withoutRequeue(m stateMachine) stateMachine {
	edges := make(map[string][]string, len(m.edges))
	for from, next := range m.edges {
		kept := make([]string, 0, len(next))
		for _, to := range next {
			if m.relation == "tasks" && from == "running" && to == "scheduled" {
				continue
			}
			kept = append(kept, to)
		}
		edges[from] = kept
	}
	return stateMachine{relation: m.relation, edges: edges}
}

// findCycle returns a state on a cycle, or "" when the machine is acyclic.
func findCycle(m stateMachine) string {
	const (
		open = 1
		done = 2
	)
	mark := map[string]int{}
	var walk func(string) string
	walk = func(state string) string {
		switch mark[state] {
		case open:
			return state
		case done:
			return ""
		}
		mark[state] = open
		for _, next := range m.edges[state] {
			if found := walk(next); found != "" {
				return found
			}
		}
		mark[state] = done
		return ""
	}
	for _, state := range m.states() {
		if found := walk(state); found != "" {
			return found
		}
	}
	return ""
}

func reachableFrom(m stateMachine, start string) map[string]bool {
	seen := map[string]bool{start: true}
	queue := []string{start}
	for len(queue) > 0 {
		state := queue[0]
		queue = queue[1:]
		for _, next := range m.edges[state] {
			if !seen[next] {
				seen[next] = true
				queue = append(queue, next)
			}
		}
	}
	return seen
}

func canReachTerminal(m stateMachine, start string) bool {
	for state := range reachableFrom(m, start) {
		if m.terminal(state) {
			return true
		}
	}
	return false
}
