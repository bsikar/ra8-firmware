//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"sort"
	"testing"
)

// escapedPIDs is the sorted pid list of a walk's answer, so an assertion can
// state the set without depending on discovery order.
func escapedPIDs(processes []treeProcess) []int {
	pids := make([]int, 0, len(processes))
	for _, process := range processes {
		pids = append(pids, process.PID)
	}
	sort.Ints(pids)
	return pids
}

func sameInts(got, want []int) bool {
	if len(got) != len(want) {
		return false
	}
	for index := range got {
		if got[index] != want[index] {
			return false
		}
	}
	return true
}

func TestGroupSignalReachesItsOwnMembers(t *testing.T) {
	member := treeProcess{PID: 11, PPID: 10, PGID: 10}
	if !reachedByTheGroupSignal(member, 10) {
		t.Fatalf("a process in the signalled group was not treated as covered")
	}
}

func TestGroupSignalDoesNotReachAnotherGroup(t *testing.T) {
	escapee := treeProcess{PID: 13, PPID: 10, PGID: 13}
	if reachedByTheGroupSignal(escapee, 10) {
		t.Fatalf("a process outside the signalled group was treated as covered")
	}
}

// The signalled group is named by the leader's pid, not by whatever group the
// leader now sits in. A descendant sharing the leader's NEW group is reached
// by nothing: the group signal goes to -leader, which it left.
func TestGroupSignalIsJudgedByTheSignalledGroupNotTheLeadersOwnGroup(t *testing.T) {
	movedWithTheLeader := treeProcess{PID: 11, PPID: 10, PGID: 500}
	if reachedByTheGroupSignal(movedWithTheLeader, 10) {
		t.Fatalf("a descendant in the leader's new group was treated as covered by a signal to the old one")
	}
	if !reachedByTheGroupSignal(movedWithTheLeader, 500) {
		t.Fatalf("a descendant was not covered by a signal to the group it is actually in")
	}
}

// The ordinary case: Setpgid made the leader a group named by its own pid, so
// judging against the signalled group answers exactly as before.
func TestSettledLeaderClassifiesDescendantsUnchanged(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 10},  // the step leader
		11: {PID: 11, PPID: 10, PGID: 10}, // ordinary child
		12: {PID: 12, PPID: 11, PGID: 10}, // ordinary grandchild
		13: {PID: 13, PPID: 10, PGID: 13}, // called setsid
		14: {PID: 14, PPID: 13, PGID: 13}, // child of the escapee
	}
	got := escapedPIDs(escapedDescendants(10, table))
	if want := []int{13, 14}; !sameInts(got, want) {
		t.Fatalf("escaped set %v, want %v", got, want)
	}
}

// The regression this closes. The leader moved itself into group 500 and then
// forked: pid 11 and 12 are the step's real work and a signal to -10 cannot
// reach them, so they belong in the sweep.
func TestDescendantsThatMovedWithTheLeaderAreSwept(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 500},  // the leader, after setsid
		11: {PID: 11, PPID: 10, PGID: 500}, // forked after the move
		12: {PID: 12, PPID: 11, PGID: 500},
	}
	got := escapedPIDs(escapedDescendants(10, table))
	if want := []int{11, 12}; !sameInts(got, want) {
		t.Fatalf("escaped set %v, want %v", got, want)
	}
}

// The other half of the same inversion: a descendant left behind in the
// original group IS reached by the group signal, so signalling it by name as
// well is the double delivery the sweep avoids.
func TestDescendantLeftInTheSignalledGroupIsNotSwept(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 500}, // the leader, after setsid
		11: {PID: 11, PPID: 10, PGID: 10}, // forked before the move
	}
	if got := escapedDescendants(10, table); got != nil {
		t.Fatalf("swept a descendant the group signal already reaches: %v", escapedPIDs(got))
	}
}

// A leader that moved keeps a mixed tree readable: only the half the signal
// misses is swept.
func TestMixedTreeUnderAMovedLeaderSweepsOnlyTheUnreachedHalf(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 500},
		11: {PID: 11, PPID: 10, PGID: 10},  // still in the signalled group
		12: {PID: 12, PPID: 10, PGID: 500}, // moved with the leader
		13: {PID: 13, PPID: 12, PGID: 13},  // setsid of its own
		20: {PID: 20, PPID: 1, PGID: 500},  // unrelated, same group, not a descendant
	}
	got := escapedPIDs(escapedDescendants(10, table))
	if want := []int{12, 13}; !sameInts(got, want) {
		t.Fatalf("escaped set %v, want %v", got, want)
	}
}

// Group membership is not descent. A process sharing the leader's group but
// sitting outside its subtree is not the step's to kill.
func TestUnrelatedProcessSharingTheGroupIsNotSwept(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 10},
		20: {PID: 20, PPID: 1, PGID: 99},
		21: {PID: 21, PPID: 20, PGID: 99},
	}
	if got := escapedDescendants(10, table); got != nil {
		t.Fatalf("swept a process outside the leader's subtree: %v", escapedPIDs(got))
	}
}

func TestMovedLeaderIsStillRequiredToExist(t *testing.T) {
	table := map[int]treeProcess{
		11: {PID: 11, PPID: 10, PGID: 500},
	}
	if got := escapedDescendants(10, table); got != nil {
		t.Fatalf("walked a tree whose leader is gone: %v", escapedPIDs(got))
	}
}

func TestMovedLeaderIsNotSweptAsItsOwnEscapee(t *testing.T) {
	table := map[int]treeProcess{
		10: {PID: 10, PPID: 1, PGID: 500},
	}
	for _, process := range escapedDescendants(10, table) {
		if process.PID == 10 {
			t.Fatalf("the step leader was swept as an escapee")
		}
	}
}

// The bound holds whatever group the leader is in: a runaway tree under a
// moved leader is now entirely escaped, and the walk must still stop.
func TestSweepUnderAMovedLeaderStaysBounded(t *testing.T) {
	table := map[int]treeProcess{10: {PID: 10, PPID: 1, PGID: 500}}
	for pid := 11; pid < 11+maxTreeProcesses*2; pid++ {
		table[pid] = treeProcess{PID: pid, PPID: pid - 1, PGID: 500}
	}
	escaped := escapedDescendants(10, table)
	if len(escaped) > maxTreeProcesses {
		t.Fatalf("sweep returned %d processes, bound is %d", len(escaped), maxTreeProcesses)
	}
	if len(escaped) == 0 {
		t.Fatalf("sweep found nothing under a moved leader")
	}
}
