// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// The two ways the service account can be locked out of its own state, and
// what the store has to do about each.
//
// Every other refusal in this store is decided by reading the record. These
// two are decided by the account's standing on the filesystem, which the
// checks before them cannot see: a mode the stat call is happy with and the
// open still refuses, and a directory the store may read but may not write.
// Both have to end as ErrUnsafeState, because a board agent that treats
// either as "no record yet" would acknowledge a grant it never durably
// recorded.

// withoutRootPrivileges skips a test that only means something for an account
// the kernel actually holds to a file mode.
func withoutRootPrivileges(t *testing.T) {
	t.Helper()
	if os.Geteuid() == 0 {
		t.Skip("file modes do not refuse root, so there is nothing to test here")
	}
}

// A record the account may stat but may not read is refused, not read as an
// absent record. Mode 0o000 passes every check the load makes before the open
// (it is a regular file, it is within the size bounds, and it grants nothing
// to the group or to the world) and then refuses the open itself.
func TestLoadRefusesARecordTheAccountCannotOpen(t *testing.T) {
	withoutRootPrivileges(t)
	directory := privateStateDirectory(t)
	path := filepath.Join(directory, "generation.state")
	store, err := NewFileHighWater(path, "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Advance(7); err != nil {
		t.Fatal(err)
	}
	if got, err := store.Load(); err != nil || got != 7 {
		t.Fatalf("the record read back as %d, %v before it was closed off", got, err)
	}
	if err := os.Chmod(path, 0o000); err != nil {
		t.Fatal(err)
	}

	got, err := store.Load()
	if !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("a record that cannot be opened answered %v, want an unsafe state", err)
	}
	if got != 0 {
		t.Fatalf("answered generation %d alongside its refusal", got)
	}
}

// A directory the account may read but may not write cannot take the
// temporary record the write is staged through. The store has to refuse
// rather than report a generation it never wrote, and it must leave the
// existing record alone.
func TestAdvanceRefusesADirectoryThatWillNotTakeATemporaryRecord(t *testing.T) {
	withoutRootPrivileges(t)
	directory := privateStateDirectory(t)
	path := filepath.Join(directory, "generation.state")
	store, err := NewFileHighWater(path, "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	// The lock is taken before the write is staged, and taking it creates a
	// file of its own. Plant it while the directory still accepts one, so
	// that the refusal under test is the staged record and not the lock.
	lock, err := os.OpenFile(path+".lock", os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		t.Fatal(err)
	}
	if err := lock.Close(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })
	if err := os.Chmod(directory, 0o500); err != nil {
		t.Fatal(err)
	}

	if err := store.Advance(3); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("a directory that will not take a staged record answered %v, want an unsafe state", err)
	}
	if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("the refused write left a record behind: %v", err)
	}
}
