//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"os"
	"os/exec"
	"reflect"
	"syscall"
	"testing"
)

func escapee(pid int, start uint64) treeProcess {
	return treeProcess{PID: pid, PPID: 2, PGID: pid, StartTicks: start}
}

func pidsOf(processes []treeProcess) []int {
	pids := make([]int, 0, len(processes))
	for _, process := range processes {
		pids = append(pids, process.PID)
	}
	return pids
}

func holds(processes []treeProcess, pid int) bool {
	for _, process := range processes {
		if process.PID == pid {
			return true
		}
	}
	return false
}

// A process discovered in both readings is one process and is signalled once.
func TestMergeKeepsOneEntryPerIdentity(t *testing.T) {
	before := []treeProcess{escapee(10, 100), escapee(11, 101)}
	during := []treeProcess{escapee(11, 101), escapee(12, 102)}

	merged := mergeEscapedDescendants(before, during)

	if got, want := pidsOf(merged), []int{10, 11, 12}; !reflect.DeepEqual(got, want) {
		t.Fatalf("merged pids = %v, want %v", got, want)
	}
}

// The second reading is the whole point of the refresh: a descendant that left
// the group during the grace window is in it and in nothing else.
func TestMergeKeepsWhatOnlyTheSecondReadingSaw(t *testing.T) {
	before := []treeProcess{escapee(10, 100)}
	during := []treeProcess{escapee(10, 100), escapee(77, 777)}

	merged := mergeEscapedDescendants(before, during)

	if !holds(merged, 77) {
		t.Fatalf("merged = %v, want the escapee found during the grace window", pidsOf(merged))
	}
}

// A pid reused between the two readings is two processes. Both entries are
// kept, because signalEscapedDescendants is what decides which one is live and
// dropping either here would decide it on worse evidence.
func TestMergeKeepsBothEntriesForARecycledPID(t *testing.T) {
	before := []treeProcess{escapee(10, 100)}
	during := []treeProcess{escapee(10, 900)}

	merged := mergeEscapedDescendants(before, during)

	if len(merged) != 2 {
		t.Fatalf("merged = %v, want both readings of pid 10 kept", merged)
	}
	if merged[0].StartTicks != 100 || merged[1].StartTicks != 900 {
		t.Fatalf("merged start ticks = %d,%d, want 100,900", merged[0].StartTicks, merged[1].StartTicks)
	}
}

// Nothing known before SIGTERM is dropped by the refresh, whatever the second
// reading holds, including an empty one.
func TestMergeNeverDropsTheFirstReading(t *testing.T) {
	before := []treeProcess{escapee(10, 100), escapee(11, 101), escapee(12, 102)}

	for name, during := range map[string][]treeProcess{
		"nil second reading":       nil,
		"empty second reading":     {},
		"disjoint second reading":  {escapee(20, 200)},
		"identical second reading": {escapee(10, 100), escapee(11, 101), escapee(12, 102)},
	} {
		t.Run(name, func(t *testing.T) {
			merged := mergeEscapedDescendants(before, during)
			for _, process := range before {
				if !holds(merged, process.PID) {
					t.Fatalf("merged = %v, want pid %d kept", pidsOf(merged), process.PID)
				}
			}
		})
	}
}

// Order is first-seen: the pre-SIGTERM reading leads, so the processes known
// longest are signalled first.
func TestMergePreservesFirstSeenOrder(t *testing.T) {
	before := []treeProcess{escapee(30, 300), escapee(10, 100)}
	during := []treeProcess{escapee(20, 200), escapee(30, 300), escapee(5, 50)}

	merged := mergeEscapedDescendants(before, during)

	if got, want := pidsOf(merged), []int{30, 10, 20, 5}; !reflect.DeepEqual(got, want) {
		t.Fatalf("merged pids = %v, want %v", got, want)
	}
}

// Either side empty, or both, is an ordinary outcome and not a failure.
func TestMergeHandlesEmptyReadings(t *testing.T) {
	one := []treeProcess{escapee(10, 100)}
	for name, test := range map[string]struct {
		first, second []treeProcess
		want          int
	}{
		"both nil":    {nil, nil, 0},
		"first only":  {one, nil, 1},
		"second only": {nil, one, 1},
		"both empty":  {[]treeProcess{}, []treeProcess{}, 0},
	} {
		t.Run(name, func(t *testing.T) {
			if got := len(mergeEscapedDescendants(test.first, test.second)); got != test.want {
				t.Fatalf("len(merged) = %d, want %d", got, test.want)
			}
		})
	}
}

// The merged list carries the same ceiling one walk does, so two readings of a
// forking tree cannot hand the kill loop an unbounded list.
func TestMergeIsBoundedByTheWalkCeiling(t *testing.T) {
	var before, during []treeProcess
	for index := 0; index < maxTreeProcesses; index++ {
		before = append(before, escapee(index+2, uint64(index)))
	}
	for index := 0; index < 64; index++ {
		during = append(during, escapee(1_000_000+index, uint64(index)))
	}

	merged := mergeEscapedDescendants(before, during)

	if len(merged) != maxTreeProcesses {
		t.Fatalf("len(merged) = %d, want %d", len(merged), maxTreeProcesses)
	}
}

// The merge builds its own list, so appending to the result later cannot write
// into the snapshot the teardown still holds.
func TestMergeDoesNotAliasItsInputs(t *testing.T) {
	before := []treeProcess{escapee(10, 100)}
	during := []treeProcess{escapee(11, 101)}

	merged := mergeEscapedDescendants(before, during)
	merged[0] = escapee(99, 999)

	if before[0].PID != 10 || during[0].PID != 11 {
		t.Fatalf("inputs mutated: before=%v during=%v", before, during)
	}
}

// The refreshed list is a real walk: a live child in its own process group is
// discovered by escapeesBeforeTheKill with nothing known beforehand, which is
// the shape of a descendant that left the group after the first snapshot.
func TestEscapeesBeforeTheKillFindsAChildOutsideTheGroup(t *testing.T) {
	child := groupedSleeper(t)

	found := escapeesBeforeTheKill(os.Getpid(), nil)

	if !holds(found, child) {
		t.Fatalf("escapees = %v, want the setpgid child %d", pidsOf(found), child)
	}
}

// A leader with no descendants at all leaves the pre-SIGTERM reading exactly
// as it was, so the refresh can only ever add.
func TestEscapeesBeforeTheKillKeepsTheKnownListWhenTheWalkFindsNothing(t *testing.T) {
	known := []treeProcess{escapee(10, 100), escapee(11, 101)}

	found := escapeesBeforeTheKill(groupedSleeper(t), known)

	if got, want := pidsOf(found), []int{10, 11}; !reflect.DeepEqual(got, want) {
		t.Fatalf("escapees = %v, want %v", got, want)
	}
}

// groupedSleeper starts a childless process in its own process group and
// returns its pid. The whole group is torn down when the test ends, so no
// descendant of this test outlives it and loads the host for the steps the
// rest of the package times.
func groupedSleeper(t *testing.T) int {
	t.Helper()
	command := exec.Command("sleep", "30")
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := command.Start(); err != nil {
		t.Skipf("cannot start a child here: %v", err)
	}
	pid := command.Process.Pid
	t.Cleanup(func() {
		_ = syscall.Kill(-pid, syscall.SIGKILL)
		_ = syscall.Kill(pid, syscall.SIGKILL)
		_ = command.Wait()
	})
	return pid
}
