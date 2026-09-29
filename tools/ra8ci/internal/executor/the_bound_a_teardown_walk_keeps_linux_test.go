// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//go:build linux

package executor

import (
	"testing"
)

// A teardown walks the process tree of a step that is already being killed,
// which is exactly when the tree is least trustworthy: a fork bomb, a cycle
// written into the parent pointers, or a table far larger than any real host
// has. The walk is not expected to win against any of those. It is expected
// not to hang on them, and to keep answering honestly about what it did see.

// aForkBomb builds a synthetic table of one leader and count descendants in a
// single chain, all outside the signalled group so every one of them would be
// reported if the walk had no bound.
func aForkBomb(leader, count int) map[int]treeProcess {
	table := map[int]treeProcess{leader: {PID: leader, PPID: 1, PGID: leader}}
	for i := 0; i < count; i++ {
		pid := leader + 1 + i
		parent := leader
		if i > 0 {
			parent = pid - 1
		}
		table[pid] = treeProcess{PID: pid, PPID: parent, PGID: pid}
	}
	return table
}

// A tree larger than the walk's bound is cut off at the bound rather than
// followed to its end. The teardown still signals what it found; it simply
// stops discovering, which is the trade a host being torn down needs.
func TestAWalkStopsAtItsBoundRatherThanFollowingAForkBomb(t *testing.T) {
	const leader = 1000
	escaped := escapedDescendants(leader, aForkBomb(leader, 5000))

	if len(escaped) == 0 {
		t.Fatal("a bounded walk reported nothing at all")
	}
	if len(escaped) > maxTreeProcesses {
		t.Fatalf("escaped = %d, want no more than the walk's bound of %d",
			len(escaped), maxTreeProcesses)
	}
	// Everything reported is a real member of the table, in the chain
	// below the leader: a bound that dropped its honesty would be worse
	// than one that walked too far.
	seen := map[int]bool{}
	for _, process := range escaped {
		if process.PID <= leader {
			t.Fatalf("escaped process %d is not below the leader", process.PID)
		}
		if seen[process.PID] {
			t.Fatalf("process %d reported twice", process.PID)
		}
		seen[process.PID] = true
	}
}

// A tree comfortably inside the bound is walked to its end, so the bound is
// a ceiling on a pathological host and not a cut every teardown takes.
func TestAWalkInsideTheBoundIsFollowedToItsEnd(t *testing.T) {
	const leader = 2000
	const descendants = 64
	escaped := escapedDescendants(leader, aForkBomb(leader, descendants))

	if len(escaped) != descendants {
		t.Fatalf("escaped = %d, want every one of the %d descendants", len(escaped), descendants)
	}
}

// Parent pointers come from a table read one process at a time while the tree
// is changing, so a cycle in them is a state the walk can genuinely be handed.
// It has to end, and it must not report the leader as its own descendant.
func TestAWalkEndsOnATreeThatPointsBackAtItself(t *testing.T) {
	const leader = 3000
	table := map[int]treeProcess{
		leader:     {PID: leader, PPID: 1, PGID: leader},
		leader + 1: {PID: leader + 1, PPID: leader, PGID: leader + 1},
		leader + 2: {PID: leader + 2, PPID: leader + 1, PGID: leader + 2},
	}
	// The cycle: the leader is claimed as a child of its own grandchild.
	table[leader] = treeProcess{PID: leader, PPID: leader + 2, PGID: leader}

	escaped := escapedDescendants(leader, table)
	if len(escaped) != 2 {
		t.Fatalf("escaped = %+v, want the two descendants once each", escaped)
	}
	for _, process := range escaped {
		if process.PID == leader {
			t.Fatal("the leader was reported as its own descendant")
		}
	}
}

// The walk reports what the group signal will not reach, and nothing else:
// a descendant still in the signalled group is already covered, and
// signalling it again would race the group teardown for no gain.
func TestAWalkReportsOnlyWhatTheGroupSignalMisses(t *testing.T) {
	const leader = 4000
	table := map[int]treeProcess{
		leader: {PID: leader, PPID: 1, PGID: leader},
		// In the signalled group, which is the leader's own pid.
		leader + 1: {PID: leader + 1, PPID: leader, PGID: leader},
		// Escaped into a group of its own.
		leader + 2: {PID: leader + 2, PPID: leader, PGID: leader + 2},
		// Escaped, and only reachable through the covered child, so the
		// walk has to keep descending past a process it does not report.
		leader + 3: {PID: leader + 3, PPID: leader + 1, PGID: leader + 3},
		// Another process's child entirely.
		leader + 4: {PID: leader + 4, PPID: 1, PGID: leader + 4},
	}

	escaped := escapedDescendants(leader, table)
	reported := map[int]bool{}
	for _, process := range escaped {
		reported[process.PID] = true
	}
	if len(escaped) != 2 || !reported[leader+2] || !reported[leader+3] {
		t.Fatalf("escaped = %+v, want only the two outside the signalled group", escaped)
	}
}

// Nothing to walk is answered as nothing, in each of the three ways a
// teardown can be handed a tree it cannot use.
func TestAWalkWithNoTreeToFollowAnswersNothing(t *testing.T) {
	live := map[int]treeProcess{10: {PID: 10, PPID: 1, PGID: 10}}

	for _, absent := range []struct {
		named  string
		leader int
		table  map[int]treeProcess
	}{
		{"no leader worth signalling", 1, live},
		{"a leader of zero", 0, live},
		{"an empty table", 10, map[int]treeProcess{}},
		{"a leader the table never held", 99, live},
	} {
		t.Run(absent.named, func(t *testing.T) {
			if escaped := escapedDescendants(absent.leader, absent.table); escaped != nil {
				t.Fatalf("escaped = %+v, want nothing", escaped)
			}
		})
	}
}
