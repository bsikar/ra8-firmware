//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

// A step whose program cannot be started never ran, and the caller has to be
// told that rather than handed a result. The failure worth guarding against
// is a start error read as an ordinary nonzero exit: a task that was never
// launched would then be reported as a task that ran and failed, which is a
// different thing to debug.
func TestAStepThatCannotBeStartedIsAnErrorNotAnExit(t *testing.T) {
	absent := filepath.Join(t.TempDir(), "no-such-program")

	result, err := runCommand(context.Background(), absent, nil, t.TempDir(), []string{"PATH=/usr/bin"}, io.Discard, io.Discard, time.Second)
	if err == nil {
		t.Fatal("a program that is not there was started")
	}
	if !strings.Contains(err.Error(), "start child") {
		t.Fatalf("the refusal does not name the start: %v", err)
	}
	if result.ExitCode != -1 {
		t.Fatalf("ExitCode = %d, want -1: a step that never ran has no exit status", result.ExitCode)
	}
	if result.TimedOut || result.Cancelled {
		t.Fatalf("a start failure was reported as a deadline or a cancellation: %+v", result)
	}
}

// The exit observation is what the teardown waits on, so a kernel that
// refuses the question has to be carried back. Swallowing the refusal would
// read as the child having been observed to exit, and the teardown would go
// on to signal and reap a process it never actually watched.
func TestAnExitTheKernelWillNotReportIsCarriedBack(t *testing.T) {
	// waitid answers only for our own children. Process 1 is never one of
	// them and is always alive, so the refusal is the kernel declining the
	// relationship rather than a pid that happens to be absent.
	if err := observeUnreapedExit(1); err == nil {
		t.Fatal("the kernel was said to have reported an exit for a process we never started")
	} else if !isChildRefusal(err) {
		t.Fatalf("err = %v, want the kernel's own refusal", err)
	}

	// Our own pid is alive and is not our child either, so the same refusal
	// holds for a process that certainly exists.
	if err := observeUnreapedExit(os.Getpid()); err == nil {
		t.Fatal("the observer claimed our own process had exited")
	} else if !isChildRefusal(err) {
		t.Fatalf("err = %v, want the kernel's own refusal", err)
	}
}

func isChildRefusal(err error) bool {
	errno, ok := err.(syscall.Errno)
	return ok && (errno == syscall.ECHILD || errno == syscall.ESRCH || errno == syscall.EINVAL)
}
