//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// The cancel that arrives while the child is still finding its feet is the
// one worth pinning. A step is torn down through its process group, and the
// group only exists once the kernel has forked the leader, so the interesting
// moments are the first few milliseconds of an attempt rather than the steady
// state. A teardown that works at rest and loses the child at t=2ms hands the
// plane a runner that is still busy.
//
// Each case starts a step that would outlive any test by a wide margin, lets
// the cancel land at a different point in that window, and then asks two
// questions the caller actually depends on: did the call hand the runner back
// promptly, and is the process it started genuinely gone.
func TestACancelAnywhereInTheStartupWindowStillEndsTheStep(t *testing.T) {
	millisecond := time.Millisecond
	for _, arrival := range []time.Duration{
		0, millisecond, 2 * millisecond, 5 * millisecond,
		20 * millisecond, 80 * millisecond, 250 * millisecond,
	} {
		t.Run(arrival.String(), func(t *testing.T) {
			root := t.TempDir()
			pidPath := filepath.Join(root, "step.pid")
			// The step records its own pid and then sleeps far past the end
			// of the test: nothing but the cancellation can end it in time.
			script := fmt.Sprintf("echo $$ > %q\nsleep 300\n", pidPath)

			ctx, cancel := context.WithCancel(context.Background())
			type outcome struct {
				result commandResult
				err    error
				took   time.Duration
			}
			answered := make(chan outcome, 1)
			started := time.Now()
			go func() {
				result, err := runCommand(ctx, "/bin/sh", []string{"-c", script},
					root, cleanTestEnvironment(), io.Discard, io.Discard, 50*time.Millisecond)
				answered <- outcome{result, err, time.Since(started)}
			}()

			time.Sleep(arrival)
			cancel()

			var answer outcome
			select {
			case answer = <-answered:
			case <-time.After(30 * time.Second):
				// Leaving the sleeper behind would poison every later test on
				// this box, so it is put down before the failure is reported.
				killRecordedStep(pidPath)
				t.Fatalf("a cancel at %v never handed the runner back: the step outlived it", arrival)
			}
			if answer.err != nil {
				t.Fatalf("cancelled step reported an error: %v", answer.err)
			}
			if !answer.result.Cancelled || answer.result.TimedOut {
				t.Fatalf("cancelled step is not reported as cancelled: %+v", answer.result)
			}
			// The sleeper would have run for five minutes. Anything close to
			// that means the teardown waited the step out rather than ending it.
			if answer.took > 30*time.Second {
				t.Fatalf("a cancel at %v took %v to hand the runner back", arrival, answer.took)
			}
			// A cancel that returns while the step is still running is the
			// failure this whole case exists to catch: the caller is told the
			// runner is free while the guest is still on it.
			if pid, ok := recordedStep(pidPath); ok && processAlive(pid) {
				killRecordedStep(pidPath)
				t.Fatalf("step process %d was still alive when the cancel returned", pid)
			}
		})
	}
}

// TestAStepLeftAloneRunsToItsOwnEnd is the other half of the case above.
// Without it, a fixture whose script never ran at all would satisfy every
// assertion there, and "the step is gone" would prove nothing.
func TestAStepLeftAloneRunsToItsOwnEnd(t *testing.T) {
	root := t.TempDir()
	pidPath := filepath.Join(root, "step.pid")
	result, err := runCommand(context.Background(), "/bin/sh",
		[]string{"-c", fmt.Sprintf("echo $$ > %q\nexit 7\n", pidPath)},
		root, cleanTestEnvironment(), io.Discard, io.Discard, 50*time.Millisecond)
	if err != nil {
		t.Fatalf("uncancelled step reported an error: %v", err)
	}
	if result.Cancelled || result.TimedOut {
		t.Fatalf("nobody cancelled this step: %+v", result)
	}
	if result.ExitCode != 7 {
		t.Fatalf("exit code = %d, want the 7 the script chose", result.ExitCode)
	}
	if _, ok := recordedStep(pidPath); !ok {
		t.Fatal("the script never recorded a pid, so the fixture never ran it")
	}
}

// recordedStep reads the pid the step wrote for itself. A step cancelled
// before the shell reached its first line never writes one, which is an
// honest answer rather than a failure.
func recordedStep(pidPath string) (int, bool) {
	data, err := os.ReadFile(pidPath)
	if err != nil {
		return 0, false
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil || pid <= 1 {
		return 0, false
	}
	return pid, true
}

// processAlive answers whether the pid still names a live process. Signal 0
// checks for existence only, and runCommand reaps the leader, so a torn-down
// step is gone rather than a zombie that still answers.
func processAlive(pid int) bool {
	return syscall.Kill(pid, 0) == nil
}

func killRecordedStep(pidPath string) {
	if pid, ok := recordedStep(pidPath); ok {
		_ = syscall.Kill(-pid, syscall.SIGKILL)
		_ = syscall.Kill(pid, syscall.SIGKILL)
	}
}
