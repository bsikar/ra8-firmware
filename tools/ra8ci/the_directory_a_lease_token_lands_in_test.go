//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"os"
	"path/filepath"
	"testing"
)

// A board lease token is a bearer credential for a piece of hardware: whoever
// holds it can extend, checkpoint and release a lease that another agent is
// waiting on. So the directory it lands in is part of the credential, and
// writeBoardLeaseToken judges the directory before it writes rather than
// trusting one it was handed. The companion cases in
// the_lease_token_this_machine_keeps_test.go take the token's own shape and
// the paths it refuses to read back; these two take the directory's, which is
// the half that decides who else on the machine can read the token once it is
// written.

// TestWriteBoardLeaseTokenRefusesADirectoryItCannotCreate pins the refusal
// that comes before any judgement of the directory: a path that cannot be a
// directory at all. A parent that is a regular file is the ordinary way this
// happens, an operator pointing the lease directory inside a file rather than
// beside it, and the refusal names the creating rather than the token, since
// the token was never the problem.
func TestWriteBoardLeaseTokenRefusesADirectoryItCannotCreate(t *testing.T) {
	occupied := filepath.Join(t.TempDir(), "not-a-directory")
	if err := os.WriteFile(occupied, []byte("a file sits here"), 0o600); err != nil {
		t.Fatalf("plant file: %v", err)
	}
	err := writeBoardLeaseToken(filepath.Join(occupied, "leases"), soundLeaseToken(t))
	if err == nil {
		t.Fatal("a lease directory beneath a regular file was accepted")
	}
	if err.Error() != "create private board lease directory" {
		t.Fatalf("refusal = %q, want it to name the creating", err)
	}
}

// TestWriteBoardLeaseTokenRefusesADirectoryTheMachineCanAlreadyRead pins the
// arm that only an EXISTING directory reaches. A directory this code creates
// is 0700, so the permission check can only ever fail on one somebody else
// made, and MkdirAll leaves an existing directory's mode alone rather than
// tightening it. That is the whole reason the check exists after the create
// instead of being folded into it, and it is worth pinning in both
// directions: a group-readable directory is refused and nothing is written,
// and the same directory tightened to 0700 is then accepted.
func TestWriteBoardLeaseTokenRefusesADirectoryTheMachineCanAlreadyRead(t *testing.T) {
	token := soundLeaseToken(t)
	for name, mode := range map[string]os.FileMode{
		"readable by the group":   0o750,
		"readable by everybody":   0o755,
		"writable by everybody":   0o707,
		"executable by everybody": 0o701,
	} {
		t.Run(name, func(t *testing.T) {
			directory := filepath.Join(t.TempDir(), "leases")
			if err := os.Mkdir(directory, 0o700); err != nil {
				t.Fatalf("plant directory: %v", err)
			}
			if err := os.Chmod(directory, mode); err != nil {
				t.Fatalf("loosen directory: %v", err)
			}
			err := writeBoardLeaseToken(directory, token)
			if err == nil {
				t.Fatalf("a %v lease directory was accepted", mode)
			}
			if err.Error() != "board lease directory must be a private, nonsymlink directory" {
				t.Fatalf("refusal = %q, want it to name the directory", err)
			}
			// A refused directory holds nothing, not even a temporary:
			// the refusal comes before the token is encoded.
			left, readErr := os.ReadDir(directory)
			if readErr != nil {
				t.Fatalf("read directory: %v", readErr)
			}
			if len(left) != 0 {
				t.Fatalf("a refused write left %d entries behind", len(left))
			}
		})
	}
}

// TestWriteBoardLeaseTokenTakesADirectoryOnceItIsPrivate is the other half of
// the case above, and the one that proves the refusal is about the mode
// rather than about the directory already existing. The same path, tightened,
// takes the token and keeps it readable by nobody else.
func TestWriteBoardLeaseTokenTakesADirectoryOnceItIsPrivate(t *testing.T) {
	token := soundLeaseToken(t)
	directory := filepath.Join(t.TempDir(), "leases")
	if err := os.Mkdir(directory, 0o755); err != nil {
		t.Fatalf("plant directory: %v", err)
	}
	if err := writeBoardLeaseToken(directory, token); err == nil {
		t.Fatal("a world-readable lease directory was accepted")
	}
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatalf("tighten directory: %v", err)
	}
	if err := writeBoardLeaseToken(directory, token); err != nil {
		t.Fatalf("a private lease directory was refused: %v", err)
	}
	stored, err := os.Stat(filepath.Join(directory, token.BoardID+".json"))
	if err != nil {
		t.Fatalf("stat stored token: %v", err)
	}
	if stored.Mode().Perm()&0o077 != 0 {
		t.Fatalf("stored token is %v, want it readable by nobody else", stored.Mode().Perm())
	}
	// The write is a rename over a temporary, so the directory holds the
	// token and nothing else once it returns.
	entries, err := os.ReadDir(directory)
	if err != nil {
		t.Fatalf("read directory: %v", err)
	}
	if len(entries) != 1 || entries[0].Name() != token.BoardID+".json" {
		t.Fatalf("directory holds %d entries, want only the token", len(entries))
	}
}

// TestWriteBoardLeaseTokenCreatesItsOwnDirectoryPrivately pins what happens
// when there is no directory yet, which is the ordinary first run on a
// machine: the directory is made, and it is made private, so the permission
// check above can never fire on one this code created.
func TestWriteBoardLeaseTokenCreatesItsOwnDirectoryPrivately(t *testing.T) {
	directory := filepath.Join(t.TempDir(), "state", "ra8ci", "leases")
	if err := writeBoardLeaseToken(directory, soundLeaseToken(t)); err != nil {
		t.Fatalf("a directory that did not exist was refused: %v", err)
	}
	info, err := os.Stat(directory)
	if err != nil {
		t.Fatalf("stat created directory: %v", err)
	}
	if info.Mode().Perm()&0o077 != 0 {
		t.Fatalf("created directory is %v, want it private", info.Mode().Perm())
	}
}
