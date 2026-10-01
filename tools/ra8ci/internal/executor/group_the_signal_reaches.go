//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

// The teardown has two instruments and they have to be aimed at the same
// thing. signalGroup carries SIGTERM and SIGKILL to the group whose id is the
// step leader's pid: runCommand starts the child with Setpgid, so the kernel
// makes the leader a group of its own named by its pid, and every signal the
// teardown sends to a group is sent to -leader and to nothing else. The
// escapee sweep exists to cover what that signal cannot reach, and so the one
// question it has to ask of each descendant is whether THAT signal will reach
// it.
//
// It asked a different question. Both walks read the leader's process entry
// at teardown time and compared each descendant against the leader's CURRENT
// PGID, then treated a match as already covered. In the ordinary case the two
// questions have the same answer, because the leader's PGID is its own pid
// and stays that way, which is why this held for as long as it did.
//
// They come apart the moment the step leader moves itself. A reviewed step is
// a bash script, and a script that calls setsid on itself, or execs into a
// wrapper that does, is in a new group whose id is no longer the pid the
// executor holds. Nothing in the kernel stops it and nothing in the catalog
// forbids it: the script is reviewed by its path, not by the groups it joins.
// After such a move the comparison inverts in both directions at once. A
// descendant that followed the leader into the new group matches the leader's
// PGID, is called covered, and is dropped from the sweep, while the group
// signal goes to -leader, a group the descendant is no longer in. It is
// signalled by neither path. Meanwhile a descendant left behind in the
// original group does NOT match the leader's new PGID, is called an escapee,
// and is signalled by name as well as by the group signal that already
// reaches it, which is the double delivery escapedDescendants says it is
// avoiding.
//
// The first half is the one that costs. Those are the processes the step
// itself spawned after moving, so they are the real work of the step: the
// compiler, the flasher, the thing holding the board's serial port. They
// survive the deadline, and the next attempt to lease that board finds the
// port busy with a process no ledger has a name for. That is the whole harm
// the sweep was written to prevent, arriving through the one shape the sweep
// mistook for safety.
//
// So membership is judged against the group the teardown will actually
// signal, which is the leader's pid, not against whatever group the leader
// has since joined. The leader's entry is still read first, because a leader
// that is no longer there means there is no tree to walk; only the question
// asked of each descendant changes.
func reachedByTheGroupSignal(process treeProcess, signalledGroup int) bool {
	return process.PGID == signalledGroup
}
