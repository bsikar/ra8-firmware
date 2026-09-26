//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

// The teardown of a step takes ONE snapshot of the descendants that have left
// the step's process group, at the moment the context ends and before anything
// is signalled. That timing is deliberate and tree_linux.go says why: a
// descendant is reparented away the instant its own parent exits, so a walk
// taken after the group starts dying finds less than one taken before it.
//
// The snapshot is then used twice: once to carry SIGTERM, and again, after the
// grace window, to carry SIGKILL. Between those two uses the step is alive and
// being asked to stop, and that is exactly the window in which a process
// leaves the group. A build script that traps SIGTERM and re-launches its
// worker under setsid, a daemon started during shutdown, a child that was
// still forking when the deadline landed: each of them is in the group at
// snapshot time, or not yet born, and outside it by the time the kill goes
// out. The group signal cannot reach them because they left the group, and the
// escapee sweep cannot reach them because it is replaying a list taken before
// they left. They survive the step, holding the CPU, the open files and, on a
// board host, the serial port, which is the whole harm the escapee sweep
// exists to prevent.
//
// So the list is refreshed once more, immediately before the kill, and the two
// readings are merged. The refresh is worth taking THERE and nowhere else: the
// grace timer firing means the exit channel has not, so the step leader is
// still alive and the kernel's parent links from it are intact, which is the
// one condition the walk needs. On the other paths the leader has already
// exited, its children are already reparented, and a second walk would find
// nothing it did not find the first time.
//
// Merging rather than replacing is the point. The first reading is the only
// one taken before SIGTERM, so it holds processes that have since become
// unreachable through the tree, and signalEscapedDescendants checks each entry
// against the live (pid, start time) pair anyway: an entry whose process is
// gone is skipped, never mis-signalled. Dropping the first reading to take the
// second would trade a snapshot that is merely stale for one that is missing
// the escapees the teardown itself created.
//
// Identity here is the pair the signal is held to, (pid, start time), not the
// pid. Two entries sharing a pid with different start times are two different
// processes, one of them already gone, and both are kept so the live one is
// not dropped on the strength of the dead one.
func mergeEscapedDescendants(first, second []treeProcess) []treeProcess {
	merged := make([]treeProcess, 0, len(first)+len(second))
	seen := make(map[treeProcess]bool, len(first)+len(second))
	for _, batch := range [][]treeProcess{first, second} {
		for _, process := range batch {
			identity := treeProcess{PID: process.PID, StartTicks: process.StartTicks}
			if seen[identity] {
				continue
			}
			if len(merged) >= maxTreeProcesses {
				return merged
			}
			seen[identity] = true
			merged = append(merged, process)
		}
	}
	return merged
}

// escapeesBeforeTheKill is the refreshed list the final SIGKILL is carried on:
// what was known before SIGTERM, plus whatever left the group while the grace
// window ran.
func escapeesBeforeTheKill(leader int, known []treeProcess) []treeProcess {
	return mergeEscapedDescendants(known, snapshotEscapedDescendants(leader))
}
