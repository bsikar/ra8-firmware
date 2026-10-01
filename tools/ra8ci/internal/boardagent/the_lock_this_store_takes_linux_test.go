//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// The lock is what makes two board-agent processes safe on one board, so its
// own path is held to the same rules as the state file. A lock reached through
// a symlink is refused by O_NOFOLLOW: following it would let a caller move the
// serialization somewhere it does not protect anything.
func TestTheStateLockRefusesALockPathItCannotTrust(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}

	elsewhere := filepath.Join(t.TempDir(), "decoy.lock")
	if err := os.WriteFile(elsewhere, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	linked := filepath.Join(directory, "linked.lock")
	if err := os.Symlink(elsewhere, linked); err != nil {
		t.Skipf("this box does not make symlinks: %v", err)
	}
	if _, err := acquireStateLock(linked); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("a symlinked lock path: err = %v, want ErrUnsafeState", err)
	}

	asDirectory := filepath.Join(directory, "lock.d")
	if err := os.Mkdir(asDirectory, 0o700); err != nil {
		t.Fatal(err)
	}
	if _, err := acquireStateLock(asDirectory); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("a directory as the lock: err = %v, want ErrUnsafeState", err)
	}

	if _, err := acquireStateLock(filepath.Join(directory, "absent", "generation.lock")); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("a lock under an absent directory: err = %v, want ErrUnsafeState", err)
	}

	readable := filepath.Join(directory, "readable.lock")
	if err := os.WriteFile(readable, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := acquireStateLock(readable); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("a group-readable lock: err = %v, want ErrUnsafeState", err)
	}
}

// The ordinary path: the lock is created on first use with private
// permissions, and releasing it lets the next acquisition through in the same
// process rather than deadlocking on a lock it already holds.
func TestTheStateLockIsCreatedPrivateAndReleased(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "generation.lock")
	unlock, err := acquireStateLock(path)
	if err != nil {
		t.Fatal(err)
	}
	info, err := os.Lstat(path)
	if err != nil {
		t.Fatal(err)
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0o077 != 0 {
		t.Fatalf("the lock was created as %v", info.Mode())
	}
	unlock()

	again, err := acquireStateLock(path)
	if err != nil {
		t.Fatalf("the lock could not be taken again after release: %v", err)
	}
	again()
}

// A store whose lock path has been replaced hands the refusal back to the
// caller rather than writing without serialization.
func TestAdvanceRefusesWhenTheLockCannotBeTaken(t *testing.T) {
	state, path := newTestHighWater(t)
	if err := os.Mkdir(path+".lock", 0o700); err != nil {
		t.Fatal(err)
	}
	if err := state.Advance(3); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("advance: err = %v, want ErrUnsafeState", err)
	}
	if _, err := state.Load(); !errors.Is(err, ErrUnsafeState) {
		t.Errorf("load: err = %v, want ErrUnsafeState", err)
	}
}
