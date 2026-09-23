//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func TestCancellationStopsChildProcessGroup(t *testing.T) {
	root := t.TempDir()
	pidFile := filepath.Join(root, "child.pid")
	task := fixtureTask("spawn")
	task.Steps[0].Args = append(task.Steps[0].Args, pidFile)
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	finished := make(chan struct{})
	var runErr error
	var result Result
	go func() {
		result, runErr = runTask(ctx, root, task, io.Discard, io.Discard, 20*time.Millisecond)
		close(finished)
	}()
	var pid int
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(pidFile)
		if err == nil {
			pid, err = strconv.Atoi(string(data))
			if err != nil {
				t.Fatal(err)
			}
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if pid == 0 {
		cancel()
		<-finished
		t.Fatal("spawned child PID was not recorded")
	}
	t.Cleanup(func() { _ = syscall.Kill(pid, syscall.SIGKILL) })
	cancel()
	select {
	case <-finished:
	case <-time.After(2 * time.Second):
		t.Fatal("cancellation did not finish")
	}
	if runErr != nil || !result.Cancelled || len(result.Steps) != 1 {
		t.Fatalf("result = %+v, error = %v", result, runErr)
	}
	for time.Now().Before(deadline.Add(time.Second)) {
		data, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "stat"))
		if os.IsNotExist(err) {
			return
		}
		if err == nil {
			fields := strings.Fields(string(data))
			if len(fields) > 2 && fields[2] == "Z" {
				return
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("child process %d remained active after task cancellation", pid)
}

func TestSymlinkedCacheInsideCheckoutIsRejected(t *testing.T) {
	outer := t.TempDir()
	root := filepath.Join(outer, "checkout")
	if err := os.Mkdir(root, 0700); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(outer, "cache-link")
	if err := os.Symlink(root, link); err != nil {
		t.Fatal(err)
	}
	t.Setenv("GOCACHE", filepath.Join(link, ".cache"))
	_, err := runTask(context.Background(), root, fixtureTask("log"), io.Discard, io.Discard, 0)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("symlinked in-tree cache error = %v", err)
	}
}

func TestSuccessfulParentCannotLeaveDetachedChild(t *testing.T) {
	root := t.TempDir()
	pidFile := filepath.Join(root, "detached.pid")
	task := fixtureTask("spawn-detached")
	task.Steps[0].Args = append(task.Steps[0].Args, pidFile)
	result, err := runTask(context.Background(), root, task, io.Discard, io.Discard, 20*time.Millisecond)
	if err != nil || result.ExitCode != 0 {
		t.Fatalf("result = %+v, error = %v", result, err)
	}
	data, err := os.ReadFile(pidFile)
	if err != nil {
		t.Fatal(err)
	}
	pid, err := strconv.Atoi(string(data))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = syscall.Kill(pid, syscall.SIGKILL) })
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		data, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "stat"))
		if os.IsNotExist(err) {
			return
		}
		if err == nil {
			fields := strings.Fields(string(data))
			if len(fields) > 2 && fields[2] == "Z" {
				return
			}
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("detached child %d survived successful parent", pid)
}

func TestRunStepExecutesCheckoutRelativeProgramAndRejectsEscapingSymlink(t *testing.T) {
	root := t.TempDir()
	programPath := filepath.Join(root, "scripts", "probe.sh")
	if err := os.MkdirAll(filepath.Dir(programPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(programPath, []byte("#!/bin/sh\nprintf checkout-relative\n"), 0755); err != nil {
		t.Fatal(err)
	}
	var stdout strings.Builder
	result, err := runStep(context.Background(), root, []string{"PATH=/usr/bin:/bin"}, catalog.Step{Name: "probe", Program: "scripts/probe.sh"}, &stdout, io.Discard, time.Second)
	if err != nil || result.ExitCode != 0 || stdout.String() != "checkout-relative" {
		t.Fatalf("relative program result=%+v stdout=%q err=%v", result, stdout.String(), err)
	}
	outside := filepath.Join(t.TempDir(), "outside.sh")
	if err := os.WriteFile(outside, []byte("#!/bin/sh\nexit 0\n"), 0755); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(root, "scripts", "escape.sh")
	if err := os.Symlink(outside, link); err != nil {
		t.Fatal(err)
	}
	if _, err := resolveTaskProgram(root, "scripts/escape.sh"); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("escaping symlink error=%v", err)
	}
}
