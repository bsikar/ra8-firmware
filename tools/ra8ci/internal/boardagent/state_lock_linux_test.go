// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//go:build linux

package boardagent

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestStateLockSerializesIndependentProcesses(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	lockPath := filepath.Join(directory, "generation.lock")
	firstEntered := filepath.Join(directory, "first-entered")
	secondEntered := filepath.Join(directory, "second-entered")
	releaseFirst := filepath.Join(directory, "release-first")
	startHelper := func(mode string) *exec.Cmd {
		command := exec.Command(os.Args[0], "-test.run=^TestStateLockProcessHelper$")
		command.Env = append(os.Environ(), "RA8CI_STATE_LOCK_HELPER=1",
			"RA8CI_STATE_LOCK_MODE="+mode, "RA8CI_STATE_LOCK_PATH="+lockPath,
			"RA8CI_STATE_LOCK_MARKER="+firstEntered,
			"RA8CI_STATE_LOCK_SECOND_MARKER="+secondEntered,
			"RA8CI_STATE_LOCK_RELEASE="+releaseFirst)
		return command
	}

	first := startHelper("hold")
	if err := first.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = os.WriteFile(releaseFirst, []byte("release"), 0o600)
		_ = first.Process.Kill()
		_ = first.Wait()
	}()
	waitForFile(t, firstEntered)

	second := startHelper("acquire")
	if err := second.Start(); err != nil {
		t.Fatal(err)
	}
	time.Sleep(100 * time.Millisecond)
	if _, err := os.Stat(secondEntered); !os.IsNotExist(err) {
		t.Fatalf("second process entered while first held lock: %v", err)
	}
	if err := os.WriteFile(releaseFirst, []byte("release"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := first.Wait(); err != nil {
		t.Fatalf("first lock helper: %v", err)
	}
	if err := second.Wait(); err != nil {
		t.Fatalf("second lock helper: %v", err)
	}
	if _, err := os.Stat(secondEntered); err != nil {
		t.Fatalf("second process never entered after lock release: %v", err)
	}
}

func TestStateLockProcessHelper(t *testing.T) {
	if os.Getenv("RA8CI_STATE_LOCK_HELPER") != "1" {
		return
	}
	unlock, err := acquireStateLock(os.Getenv("RA8CI_STATE_LOCK_PATH"))
	if err != nil {
		t.Fatal(err)
	}
	defer unlock()
	mode := os.Getenv("RA8CI_STATE_LOCK_MODE")
	marker := os.Getenv("RA8CI_STATE_LOCK_MARKER")
	if mode == "acquire" {
		marker = os.Getenv("RA8CI_STATE_LOCK_SECOND_MARKER")
	}
	if err := os.WriteFile(marker, []byte(mode), 0o600); err != nil {
		t.Fatal(err)
	}
	if mode != "hold" {
		return
	}
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(os.Getenv("RA8CI_STATE_LOCK_RELEASE")); err == nil {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("parent did not release process lock")
}

func waitForFile(t *testing.T, path string) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(path); err == nil {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal(fmt.Sprintf("timed out waiting for %s", path))
}
