//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"syscall"
	"time"
	"unsafe"
)

const (
	waitIDProcess = 1
	waitExited    = 4
	waitNoWait    = 0x01000000
)

func runCommand(ctx context.Context, program string, args []string, root string, env []string, stdout, stderr io.Writer, grace time.Duration) (commandResult, error) {
	result := commandResult{ExitCode: -1}
	if cause := contextExpiration(ctx); cause != nil {
		result.TimedOut = errors.Is(cause, context.DeadlineExceeded)
		result.Cancelled = !result.TimedOut
		return result, nil
	}
	cmd := exec.Command(program, args...)
	cmd.Dir = root
	cmd.Env = env
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	cmd.WaitDelay = grace + 5*time.Second
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		return result, fmt.Errorf("start child: %w", err)
	}
	exited := make(chan error, 1)
	go func() { exited <- observeUnreapedExit(cmd.Process.Pid) }()
	var observeErr error
	var escaped []treeProcess
	select {
	case observeErr = <-exited:
		// Under host saturation both channels can be ready when this goroutine
		// is scheduled again. Never turn a child observed after its deadline
		// into success just because select chose process exit.
		if cause := contextExpiration(ctx); cause != nil {
			result.TimedOut = errors.Is(cause, context.DeadlineExceeded)
			result.Cancelled = !result.TimedOut
		}
	case <-ctx.Done():
		cause := contextExpiration(ctx)
		result.TimedOut = errors.Is(cause, context.DeadlineExceeded)
		result.Cancelled = !result.TimedOut
		// Taken before anything is signalled: a descendant that called
		// setsid is outside the group the signals below reach, and it stops
		// being discoverable through the process tree the moment its own
		// parent exits and it is reparented away.
		escaped = snapshotEscapedDescendants(cmd.Process.Pid)
		_ = signalGroup(cmd.Process.Pid, syscall.SIGTERM)
		_, _ = signalEscapedDescendants(escaped, syscall.SIGTERM)
		timer := time.NewTimer(grace)
		select {
		case observeErr = <-exited:
			timer.Stop()
		case <-timer.C:
			_ = signalGroup(cmd.Process.Pid, syscall.SIGKILL)
			_, _ = signalEscapedDescendants(escaped, syscall.SIGKILL)
			observeErr = <-exited
		}
	}
	// waitid(WNOWAIT) leaves the leader as a zombie. Its PID/PGID cannot be
	// recycled until Wait, so terminating descendants here cannot hit a new group.
	cleanupErr := signalGroup(cmd.Process.Pid, syscall.SIGKILL)
	// The same identity check runs again here, so a snapshotted pid that has
	// since exited and been recycled is skipped rather than killed.
	_, escapeErr := signalEscapedDescendants(escaped, syscall.SIGKILL)
	cleanupErr = errors.Join(cleanupErr, escapeErr)
	waitErr := cmd.Wait()
	if observeErr != nil {
		return result, fmt.Errorf("observe child exit: %w", errors.Join(observeErr, cleanupErr, waitErr))
	}
	if waitErr == nil {
		result.ExitCode = 0
	} else {
		var exitErr *exec.ExitError
		if errors.As(waitErr, &exitErr) {
			result.ExitCode = exitErr.ExitCode()
		} else {
			return result, fmt.Errorf("wait child: %w", errors.Join(waitErr, cleanupErr))
		}
	}
	if cleanupErr != nil {
		return result, fmt.Errorf("clean child process tree: %w", cleanupErr)
	}
	return result, nil
}

func observeUnreapedExit(pid int) error {
	var info [16]uint64
	for {
		_, _, errno := syscall.RawSyscall6(
			syscall.SYS_WAITID,
			waitIDProcess,
			uintptr(pid),
			uintptr(unsafe.Pointer(&info[0])),
			waitExited|waitNoWait,
			0,
			0,
		)
		if errno == 0 {
			return nil
		}
		if errno != syscall.EINTR {
			return errno
		}
	}
}

func signalGroup(pid int, signal syscall.Signal) error {
	err := syscall.Kill(-pid, signal)
	if errors.Is(err, syscall.ESRCH) {
		return nil
	}
	return err
}
