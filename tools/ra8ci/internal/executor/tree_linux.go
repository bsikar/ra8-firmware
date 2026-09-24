//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"syscall"
	"time"
)

// A process group signal reaches every descendant that stayed in the group,
// which is the ordinary case. A descendant that called setsid leaves the
// group while remaining a child in the process tree, so killing the group
// alone leaves it running past the deadline holding the guest's CPU, its
// open files and, on a board host, a serial port. This file finds those
// escapees through the kernel's own parent links and signals them by name.
//
// The identity of a process is the pair (pid, start time): Linux recycles
// pids, and a snapshot taken before the group teardown is read back after
// it, so nothing here is signalled unless it still carries the start time
// observed when it was discovered as a descendant.
const (
	// maxTreeProcesses bounds one walk. A fork bomb is not something this
	// sweep is expected to win against; it is something it must not hang on.
	maxTreeProcesses = 4096
	// treeWalkBudget bounds the whole discovery pass, including the fallback
	// scan of /proc, which has been observed taking seconds on a loaded host.
	treeWalkBudget = 250 * time.Millisecond
	// procStatFields is the number of leading fields of /proc/<pid>/stat
	// that must be present after the comm field for the walk to read
	// ppid, pgid and starttime.
	statFieldState     = 0
	statFieldPPID      = 1
	statFieldPGID      = 2
	statFieldStartTime = 19
)

// treeProcess is a process identified so a later signal cannot land on a
// recycled pid: start time is the kernel's own monotonic tiebreak.
type treeProcess struct {
	PID        int
	PPID       int
	PGID       int
	StartTicks uint64
}

// parseProcStat reads the fields the walk needs out of one /proc/<pid>/stat
// line. The comm field is unquoted and may itself contain spaces and
// parentheses, so everything is positioned from the LAST ')'.
func parseProcStat(pid int, data []byte) (treeProcess, bool) {
	close := bytes.LastIndexByte(data, ')')
	if close < 0 || close+2 >= len(data) {
		return treeProcess{}, false
	}
	fields := bytes.Fields(data[close+1:])
	if len(fields) <= statFieldStartTime {
		return treeProcess{}, false
	}
	ppid, err := strconv.Atoi(string(fields[statFieldPPID]))
	if err != nil {
		return treeProcess{}, false
	}
	pgid, err := strconv.Atoi(string(fields[statFieldPGID]))
	if err != nil {
		return treeProcess{}, false
	}
	start, err := strconv.ParseUint(string(fields[statFieldStartTime]), 10, 64)
	if err != nil {
		return treeProcess{}, false
	}
	if pid <= 0 || ppid < 0 || pgid < 0 {
		return treeProcess{}, false
	}
	return treeProcess{PID: pid, PPID: ppid, PGID: pgid, StartTicks: start}, true
}

// readProcess reads one live process. A process that exits mid-read is a
// normal outcome and reports false rather than an error.
func readProcess(pid int) (treeProcess, bool) {
	data, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "stat"))
	if err != nil {
		return treeProcess{}, false
	}
	return parseProcStat(pid, data)
}

// procTable reads every live process once. The full scan is deliberate: the
// per-task children file needs CONFIG_PROC_CHILDREN, and a teardown that
// silently finds nothing on a kernel without it is worse than one scan of
// /proc at the moment a step is already being killed.
func procTable() map[int]treeProcess {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	table := make(map[int]treeProcess, len(entries))
	for _, entry := range entries {
		pid, err := strconv.Atoi(entry.Name())
		if err != nil || pid <= 0 {
			continue
		}
		process, ok := readProcess(pid)
		if !ok {
			continue
		}
		table[pid] = process
	}
	return table
}

// escapedDescendants returns the descendants of leader that have left the
// leader's process group, so a group signal will not reach them. Members of
// the group are deliberately excluded: they are already covered, and
// signalling them twice would race the group teardown for no gain.
func escapedDescendants(leader int, table map[int]treeProcess) []treeProcess {
	if leader <= 1 || len(table) == 0 {
		return nil
	}
	root, ok := table[leader]
	if !ok {
		return nil
	}
	children := make(map[int][]int, len(table))
	for pid, process := range table {
		if pid == process.PPID {
			continue
		}
		children[process.PPID] = append(children[process.PPID], pid)
	}
	var escaped []treeProcess
	seen := map[int]bool{leader: true}
	queue := []int{leader}
	for len(queue) > 0 && len(seen) <= maxTreeProcesses {
		parent := queue[0]
		queue = queue[1:]
		for _, pid := range children[parent] {
			if seen[pid] || len(seen) > maxTreeProcesses {
				continue
			}
			seen[pid] = true
			queue = append(queue, pid)
			process := table[pid]
			if process.PGID == root.PGID {
				continue
			}
			escaped = append(escaped, process)
		}
	}
	return escaped
}

// childProcesses reads the kernel's own child list for one process. It is
// present with CONFIG_PROC_CHILDREN, reads only the subtree being torn down,
// and reports false when the kernel does not offer it.
func childProcesses(pid int) ([]int, bool) {
	tasks, err := os.ReadDir(filepath.Join("/proc", strconv.Itoa(pid), "task"))
	if err != nil {
		return nil, false
	}
	var children []int
	offered := false
	for _, task := range tasks {
		data, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "task", task.Name(), "children"))
		if err != nil {
			continue
		}
		offered = true
		for _, field := range bytes.Fields(data) {
			child, err := strconv.Atoi(string(field))
			if err != nil || child <= 1 {
				continue
			}
			children = append(children, child)
		}
	}
	return children, offered
}

// walkEscapedDescendants follows the kernel's child lists down from leader.
// It touches only the processes below the step, unlike a scan of every entry
// in /proc, and reports false when the kernel has no children file so the
// caller can fall back.
func walkEscapedDescendants(leader int) ([]treeProcess, bool) {
	root, ok := readProcess(leader)
	if !ok || leader <= 1 {
		return nil, false
	}
	first, offered := childProcesses(leader)
	if !offered {
		return nil, false
	}
	var escaped []treeProcess
	seen := map[int]bool{leader: true}
	queue := first
	for len(queue) > 0 && len(seen) <= maxTreeProcesses {
		pid := queue[0]
		queue = queue[1:]
		if seen[pid] {
			continue
		}
		seen[pid] = true
		process, ok := readProcess(pid)
		if !ok {
			continue
		}
		children, _ := childProcesses(pid)
		queue = append(queue, children...)
		if process.PGID == root.PGID {
			continue
		}
		escaped = append(escaped, process)
	}
	return escaped, true
}

// snapshotEscapedDescendants is the walk as the teardown path uses it: taken
// while the step leader is still alive, because a descendant is reparented
// away the moment its own parent exits and is then no longer discoverable
// through the tree at all.
//
// It is bounded by treeWalkBudget and runs off the teardown goroutine. A
// /proc read can block for as long as the kernel holds it, and a step that
// is already being killed must not wait on discovery: past the budget the
// snapshot is empty and the process group signal, which needs no walk at
// all, still goes out on time.
func snapshotEscapedDescendants(leader int) []treeProcess {
	walked := make(chan []treeProcess, 1)
	go func() {
		if escaped, ok := walkEscapedDescendants(leader); ok {
			walked <- escaped
			return
		}
		walked <- escapedDescendants(leader, procTable())
	}()
	timer := time.NewTimer(treeWalkBudget)
	defer timer.Stop()
	select {
	case escaped := <-walked:
		return escaped
	case <-timer.C:
		return nil
	}
}

// signalEscapedDescendants signals each snapshotted process that still holds
// the identity it was discovered with. A process that has since exited, or
// whose pid now belongs to something else, is skipped rather than signalled,
// and neither is a failure. Returns how many signals were delivered.
func signalEscapedDescendants(processes []treeProcess, signal syscall.Signal) (int, error) {
	self := os.Getpid()
	delivered := 0
	var failures error
	for _, process := range processes {
		if process.PID <= 1 || process.PID == self {
			continue
		}
		current, ok := readProcess(process.PID)
		if !ok || current.StartTicks != process.StartTicks {
			continue
		}
		err := syscall.Kill(process.PID, signal)
		switch {
		case err == nil:
			delivered++
		case errors.Is(err, syscall.ESRCH):
		default:
			failures = errors.Join(failures, fmt.Errorf("signal escaped descendant %d: %w", process.PID, err))
		}
	}
	return delivered, failures
}
