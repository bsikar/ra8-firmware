//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"os"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// The grace window exists for a step that refuses the polite signal. Nothing
// else in this package proves the timer arm is reached: every other
// cancellation test uses a child that dies of SIGTERM, so it leaves through
// the exit channel and the kill below it is never exercised.
func TestAStepThatIgnoresTheRequestToStopIsKilledAfterItsGrace(t *testing.T) {
	root := t.TempDir()
	// The shell ignores SIGTERM. Each sleep it starts does die of the group
	// signal, and the loop simply starts another, so the leader outlives the
	// request to stop without spinning the host.
	script := `trap "" TERM; while :; do /bin/sleep 1; done`
	output, err := os.OpenFile(os.DevNull, os.O_WRONLY, 0)
	if err != nil {
		t.Fatalf("open null sink: %v", err)
	}
	t.Cleanup(func() { _ = output.Close() })
	const grace = 150 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	type outcome struct {
		result commandResult
		err    error
	}
	finished := make(chan outcome, 1)
	go func() {
		result, runErr := runCommand(ctx, "/bin/sh", []string{"-c", script}, root, cleanTestEnvironment(), output, output, grace)
		finished <- outcome{result: result, err: runErr}
	}()
	// Let the loop get going, so the cancel lands on a step that is running
	// rather than on one still being started.
	select {
	case done := <-finished:
		t.Fatalf("the step ended before it was cancelled: %+v err %v", done.result, done.err)
	case <-time.After(300 * time.Millisecond):
	}
	askedAt := time.Now()
	cancel()
	select {
	case done := <-finished:
		waited := time.Since(askedAt)
		if done.err != nil {
			t.Fatalf("run: %v", done.err)
		}
		if !done.result.Cancelled || done.result.TimedOut {
			t.Fatalf("result = %+v, want cancelled and not timed out", done.result)
		}
		// Returning sooner than the grace would mean the leader took the
		// polite signal after all, and the kill arm was never reached.
		if waited < grace {
			t.Fatalf("returned %v after the cancel, want at least the %v grace", waited, grace)
		}
	case <-time.After(30 * time.Second):
		t.Fatal("cancellation never finished")
	}
}

// Teardown signals the group twice on purpose, so the second signal routinely
// arrives after the group is gone. That refusal is the expected end state, not
// a failure to report.
func TestSignalGroupTreatsAGroupThatIsAlreadyGoneAsDone(t *testing.T) {
	data, err := os.ReadFile("/proc/sys/kernel/pid_max")
	if err != nil {
		t.Skipf("pid ceiling unreadable on this host: %v", err)
	}
	ceiling, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil || ceiling < 2 {
		t.Skipf("pid ceiling unusable: %q", strings.TrimSpace(string(data)))
	}
	// A pid above the kernel's own ceiling can never name a live group. Taking
	// it from /proc rather than guessing also keeps this away from pid 1,
	// whose group signal would reach every process on the host.
	if err := signalGroup(ceiling+1, syscall.SIGKILL); err != nil {
		t.Fatalf("signalGroup over a group that cannot exist = %v, want nil", err)
	}
}
