//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"syscall"
	"testing"
	"time"
)

func TestWindowsJobClosesDescendantAfterParentExit(t *testing.T) {
	root := t.TempDir()
	pidFile := filepath.Join(root, "child.pid")
	release := filepath.Join(root, "release")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	type outcome struct {
		result commandResult
		err    error
	}
	done := make(chan outcome, 1)
	go func() {
		result, err := runCommand(ctx, os.Args[0], []string{"-test.run=^TestWindowsJobHelper$", "--", "parent", pidFile, release}, root, os.Environ(), io.Discard, io.Discard, 20*time.Millisecond)
		done <- outcome{result, err}
	}()
	var pid uint64
	for limit := time.Now().Add(2 * time.Second); time.Now().Before(limit); {
		if raw, err := os.ReadFile(pidFile); err == nil {
			pid, err = strconv.ParseUint(string(raw), 10, 32)
			if err != nil {
				t.Fatal(err)
			}
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if pid == 0 {
		t.Fatal("child PID not recorded")
	}
	child, err := syscall.OpenProcess(syscall.PROCESS_QUERY_INFORMATION|syscall.SYNCHRONIZE, false, uint32(pid))
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.CloseHandle(child)
	if err := os.WriteFile(release, []byte("go"), 0600); err != nil {
		t.Fatal(err)
	}
	select {
	case got := <-done:
		if got.err != nil || got.result.ExitCode != 0 || got.result.TimedOut || got.result.Cancelled {
			t.Fatalf("command result=%+v error=%v", got.result, got.err)
		}
	case <-time.After(4 * time.Second):
		t.Fatal("parent command did not finish")
	}
	event, err := syscall.WaitForSingleObject(child, 3000)
	if err != nil || event != syscall.WAIT_OBJECT_0 {
		t.Fatalf("descendant remained: event=%d error=%v", event, err)
	}
}

func TestWindowsJobHelper(t *testing.T) {
	sep := -1
	for i, arg := range os.Args {
		if arg == "--" {
			sep = i
			break
		}
	}
	if sep < 0 || sep+1 >= len(os.Args) {
		return
	}
	if os.Args[sep+1] == "child" {
		time.Sleep(30 * time.Second)
		os.Exit(0)
	}
	if sep+3 >= len(os.Args) {
		os.Exit(21)
	}
	pidFile, release := os.Args[sep+2], os.Args[sep+3]
	child := exec.Command(os.Args[0], "-test.run=^TestWindowsJobHelper$", "--", "child")
	child.Stdout, child.Stderr = io.Discard, io.Discard
	if err := child.Start(); err != nil {
		os.Exit(22)
	}
	if err := os.WriteFile(pidFile, []byte(strconv.Itoa(child.Process.Pid)), 0600); err != nil {
		os.Exit(23)
	}
	_ = child.Process.Release()
	for limit := time.Now().Add(2 * time.Second); time.Now().Before(limit); {
		if _, err := os.Stat(release); err == nil {
			os.Exit(0)
		}
		time.Sleep(10 * time.Millisecond)
	}
	os.Exit(24)
}
