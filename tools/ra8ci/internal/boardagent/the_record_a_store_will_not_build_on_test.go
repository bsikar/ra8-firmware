// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// privateStateDirectory is a state directory the constructor will accept. A
// test temporary directory inherits the process umask, which on a root build
// box leaves it group- and world-readable, and the constructor refuses that
// before it ever judges the path it was given.
func privateStateDirectory(t *testing.T) string {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	return directory
}

func TestNewFileHighWaterRefusesAStatePathItCannotStat(t *testing.T) {
	// An absent record is the one stat failure the constructor accepts, since
	// nothing has written the generation yet. Every other failure leaves the
	// store unable to say whether a record exists, and a store that cannot
	// tell must not be built: it would read as generation zero and authorize
	// a segment the board has already moved past.
	path := filepath.Join(privateStateDirectory(t), strings.Repeat("g", 300))
	store, err := NewFileHighWater(path, "ek-ra8d1")
	if store != nil || !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("store = %v, error = %v", store, err)
	}
}

func TestAdvanceRefusesToBuildOnARecordItCannotRead(t *testing.T) {
	directory := privateStateDirectory(t)
	path := filepath.Join(directory, "board-state")
	// Well-formed fields, a high_water that is not its own canonical form.
	// The record is refused on the read, so the advance never reaches the
	// write and the unreadable record is left exactly as it was found.
	record := "schema_version=1\nboard_id=ek-ra8d1\nhigh_water=007\n"
	if err := os.WriteFile(path, []byte(record), 0o600); err != nil {
		t.Fatal(err)
	}
	store, err := NewFileHighWater(path, "ek-ra8d1")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Advance(9); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("advance over an unreadable record = %v", err)
	}
	onDisk, err := os.ReadFile(path)
	if err != nil || string(onDisk) != record {
		t.Fatalf("record = %q, error = %v", onDisk, err)
	}
	if _, err := store.Load(); !errors.Is(err, ErrUnsafeState) {
		t.Fatalf("load after the refused advance = %v", err)
	}
	assertNoTemporaryRecord(t, directory)
}

func TestAdvanceLeavesNoTemporaryRecordBehind(t *testing.T) {
	directory := privateStateDirectory(t)
	store, err := NewFileHighWater(filepath.Join(directory, "board-state"), "ek-ra8d1")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Advance(4); err != nil {
		t.Fatal(err)
	}
	generation, err := store.Load()
	if err != nil || generation != 4 {
		t.Fatalf("generation = %d, error = %v", generation, err)
	}
	assertNoTemporaryRecord(t, directory)
}

// assertNoTemporaryRecord fails when a half-written record was left in the
// state directory. The next constructor judges every file it finds there, so
// a leftover temporary is not merely untidy.
func assertNoTemporaryRecord(t *testing.T, directory string) {
	t.Helper()
	entries, err := os.ReadDir(directory)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if strings.HasSuffix(entry.Name(), ".tmp") {
			t.Fatalf("temporary record left behind: %s", entry.Name())
		}
	}
}
