//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"os"
	"testing"
)

// noSuchPID is above any pid_max Linux will hand out, so /proc never holds
// it. It stands for a process that has already gone.
const noSuchPID = 1 << 30

// readProcess is the identity every later signal is checked against, so it
// has to answer the live process exactly and answer nothing for anything
// else. A process that cannot be read is a normal outcome, not an error.
func TestReadProcessAnswersOnlyALiveProcess(t *testing.T) {
	self, ok := readProcess(os.Getpid())
	if !ok {
		t.Fatal("this test process was not readable through /proc")
	}
	if self.PID != os.Getpid() {
		t.Fatalf("PID = %d, want %d", self.PID, os.Getpid())
	}
	if self.PPID != os.Getppid() {
		t.Fatalf("PPID = %d, want %d", self.PPID, os.Getppid())
	}
	if self.StartTicks == 0 {
		t.Fatal("start time is zero, so a recycled pid would be indistinguishable")
	}
	if self.PGID <= 0 {
		t.Fatalf("PGID = %d, want a real group", self.PGID)
	}

	for name, pid := range map[string]int{
		"gone":     noSuchPID,
		"zero":     0,
		"negative": -1,
	} {
		if process, ok := readProcess(pid); ok {
			t.Fatalf("%s: pid %d read as %+v", name, pid, process)
		}
	}
}

// procTable is the fallback the teardown falls back TO, so a kernel without
// the children file still gets a full picture. It had no test at all.
func TestProcTableReadsEveryLiveProcessOnce(t *testing.T) {
	table := procTable()
	if len(table) == 0 {
		t.Fatal("the table is empty, so the fallback would find no descendant at all")
	}

	self, ok := table[os.Getpid()]
	if !ok {
		t.Fatal("the table left out this very process")
	}
	live, ok := readProcess(os.Getpid())
	if !ok {
		t.Fatal("this test process was not readable through /proc")
	}
	if self != live {
		t.Fatalf("the table says %+v, a direct read says %+v", self, live)
	}
	if _, ok := table[1]; !ok {
		t.Fatal("the table left out pid 1")
	}

	// Every entry is keyed by its own pid, or a lookup by pid would answer
	// somebody else's identity and the start-time tiebreak would be void.
	for pid, process := range table {
		if pid <= 0 {
			t.Fatalf("the table holds pid %d", pid)
		}
		if process.PID != pid {
			t.Fatalf("entry %d carries PID %d", pid, process.PID)
		}
		if process.PPID < 0 || process.PGID < 0 {
			t.Fatalf("entry %d carries %+v", pid, process)
		}
	}
	if _, ok := table[noSuchPID]; ok {
		t.Fatal("the table invented a process")
	}
}

// childProcesses reads the kernel's own child list. It must name a live
// child, and must report false rather than an empty list when the kernel
// has nothing to offer, because those two mean different things to the
// caller: one is "no children", the other is "fall back to the full scan".
func TestChildProcessesNamesTheKernelsOwnChildren(t *testing.T) {
	sleeper := startSleeper(t)

	children, offered := childProcesses(os.Getpid())
	if !offered {
		t.Skip("this kernel does not offer /proc/<pid>/task/<tid>/children")
	}
	found := false
	for _, child := range children {
		if child == sleeper.Process.Pid {
			found = true
		}
		if child <= 1 {
			t.Fatalf("the child list holds %d", child)
		}
	}
	if !found {
		t.Fatalf("children = %v, want the sleeper %d among them", children, sleeper.Process.Pid)
	}

	if children, offered := childProcesses(noSuchPID); offered || children != nil {
		t.Fatalf("a process that does not exist offered %v", children)
	}
}

// The walk reports false, rather than an empty result, for a leader it
// cannot start from. That distinction is what makes the fallback happen at
// all: an empty result would be read as "nothing escaped".
func TestWalkEscapedDescendantsRefusesALeaderItCannotRead(t *testing.T) {
	for name, leader := range map[string]int{
		"gone":     noSuchPID,
		"zero":     0,
		"negative": -1,
		"init":     1,
	} {
		escaped, ok := walkEscapedDescendants(leader)
		if ok {
			t.Fatalf("%s: leader %d was walked, answering %+v", name, leader, escaped)
		}
		if escaped != nil {
			t.Fatalf("%s: a refused walk still answered %+v", name, escaped)
		}
	}
}

// The live half: a child that is not in the group the teardown signals is
// found through the kernel's links, carrying the start time a later signal
// will be checked against. The leader itself is never reported, or the
// teardown would signal the step it is already killing.
func TestTheWalkFindsALiveChildOutsideTheSignalledGroup(t *testing.T) {
	sleeper := startSleeper(t)
	leader := os.Getpid()

	child, ok := readProcess(sleeper.Process.Pid)
	if !ok {
		t.Fatal("the sleeper was not readable through /proc")
	}
	if reachedByTheGroupSignal(child, leader) {
		t.Skip("this sleeper shares the group the teardown would signal")
	}

	escaped, ok := walkEscapedDescendants(leader)
	if !ok {
		t.Skip("this kernel does not offer /proc/<pid>/task/<tid>/children")
	}
	var seen *treeProcess
	for index, process := range escaped {
		if process.PID == leader {
			t.Fatal("the walk reported the leader as one of its own escapees")
		}
		if process.PID == sleeper.Process.Pid {
			seen = &escaped[index]
		}
	}
	if seen == nil {
		t.Fatalf("escaped = %+v, want the sleeper %d among them", escaped, sleeper.Process.Pid)
	}
	if seen.StartTicks != child.StartTicks {
		t.Fatalf("start time = %d, want %d, or a recycled pid could be signalled",
			seen.StartTicks, child.StartTicks)
	}
}

// The snapshot is the walk as the teardown uses it. A leader that is already
// gone falls through the walk into the full-scan fallback, which cannot find
// it either, and the teardown gets an empty snapshot rather than a hang: the
// process group signal needs no walk and still goes out on time.
func TestTheSnapshotOfAGoneLeaderIsEmpty(t *testing.T) {
	for name, leader := range map[string]int{
		"gone":     noSuchPID,
		"zero":     0,
		"negative": -1,
	} {
		if escaped := snapshotEscapedDescendants(leader); len(escaped) != 0 {
			t.Fatalf("%s: leader %d answered %+v", name, leader, escaped)
		}
	}
}

// A leader with a live child answers that child, so the snapshot is really
// running the walk and not just timing out into an empty answer.
func TestTheSnapshotOfALiveLeaderCarriesItsChild(t *testing.T) {
	sleeper := startSleeper(t)
	leader := os.Getpid()

	child, ok := readProcess(sleeper.Process.Pid)
	if !ok {
		t.Fatal("the sleeper was not readable through /proc")
	}
	if reachedByTheGroupSignal(child, leader) {
		t.Skip("this sleeper shares the group the teardown would signal")
	}

	for _, process := range snapshotEscapedDescendants(leader) {
		if process.PID == sleeper.Process.Pid {
			return
		}
	}
	t.Fatalf("the snapshot of %d did not carry its child %d", leader, sleeper.Process.Pid)
}
