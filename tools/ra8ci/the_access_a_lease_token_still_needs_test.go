//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// unprivileged skips a case whose whole subject is a permission bit. Root
// walks through every mode in this file, so the assertions below would pass
// while proving the opposite of what they say.
func unprivileged(t *testing.T) {
	t.Helper()
	if os.Geteuid() == 0 {
		t.Skip("running as root, which ignores the permission bits under test")
	}
}

// The directory cases in the_directory_a_lease_token_lands_in_test.go take a
// directory this code cannot create and one the rest of the machine can
// already read. Both are judged before a byte is written. This is the arm
// after them: a directory that passes every judgement and then refuses the
// write anyway, which is what a 0500 directory does. MkdirAll is content with
// it because it already exists, and the privacy check is content with it
// because 0500 grants the group and the world nothing. Only os.CreateTemp
// finds out.
//
// The refusal has to name the temporary rather than the directory, because an
// operator told "board lease directory must be private" would tighten a
// directory that is already too tight and get the same failure again.
func TestWriteBoardLeaseTokenRefusesADirectoryItCannotWriteInto(t *testing.T) {
	unprivileged(t)
	directory := filepath.Join(t.TempDir(), "leases")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatalf("plant directory: %v", err)
	}
	if err := os.Chmod(directory, 0o500); err != nil {
		t.Fatalf("seal directory: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })

	err := writeBoardLeaseToken(directory, soundLeaseToken(t))
	if err == nil {
		t.Fatal("a sealed lease directory was written into")
	}
	if err.Error() != "create temporary board lease token" {
		t.Fatalf("refusal = %q, want it to name the temporary rather than the directory", err)
	}
}

// The mirror of the write case, on the read side. readBoardLeaseToken judges
// the token by Lstat first: regular, not a symlink, a sane size, and nothing
// granted to the group or the world. A 0000 file satisfies every one of those
// and still cannot be opened, so this is the arm that only exists because
// Lstat answers about a file's advertised shape rather than about whether
// this process may read it.
//
// It is worth its own case because the two refusals send an operator to
// different places: "unavailable or unsafe" is about the token's shape, and
// "open board lease token" is about this machine's access to it.
func TestReadBoardLeaseTokenRefusesATokenItCannotOpen(t *testing.T) {
	unprivileged(t)
	directory := filepath.Join(t.TempDir(), "leases")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatalf("plant directory: %v", err)
	}
	token := soundLeaseToken(t)
	encoded, err := json.Marshal(token)
	if err != nil {
		t.Fatalf("encode token: %v", err)
	}
	path := filepath.Join(directory, token.BoardID+".json")
	if err := os.WriteFile(path, encoded, 0o600); err != nil {
		t.Fatalf("plant token: %v", err)
	}
	if err := os.Chmod(path, 0o000); err != nil {
		t.Fatalf("seal token: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(path, 0o600) })

	if _, err := readBoardLeaseToken(directory, token.BoardID); err == nil {
		t.Fatal("an unopenable lease token was read back")
	} else if err.Error() != "open board lease token" {
		t.Fatalf("refusal = %q, want it to name the open rather than the token's shape", err)
	}

	// The same file, once this process may read it, is believed. Without
	// this half the case above would also pass against a token the reader
	// rejects for some other reason entirely.
	if err := os.Chmod(path, 0o600); err != nil {
		t.Fatalf("unseal token: %v", err)
	}
	readBack, err := readBoardLeaseToken(directory, token.BoardID)
	if err != nil {
		t.Fatalf("a readable token was refused: %v", err)
	}
	if readBack.LeaseID != token.LeaseID || readBack.BoardID != token.BoardID {
		t.Fatalf("read back %+v, want the token that was planted", readBack)
	}
}

// stubBoardHeartbeater fails the test if it is ever asked for a beat. The
// cases below are all about refusals that must land before the client is
// reached, so being called at all is the failure.
type stubBoardHeartbeater struct{ t *testing.T }

func (s stubBoardHeartbeater) Heartbeat(context.Context, boardclient.LeaseToken) (board.Snapshot,
	boardclient.HolderLiveness, error) {
	s.t.Helper()
	s.t.Fatal("an incomplete heartbeat invocation reached the client")
	return board.Snapshot{}, boardclient.HolderLiveness{}, nil
}

// heartbeatBoardLease refuses an incomplete invocation before it touches the
// lease directory. The order matters: a caller that forgot the client should
// be told so, not told that this machine holds no lease for the board, which
// is what reading first would report and which would send an operator to look
// for a token that was never the problem.
func TestHeartbeatBoardLeaseRefusesBeforeItReadsAnything(t *testing.T) {
	// A directory that would fail loudly if it were ever consulted.
	unreadable := filepath.Join(t.TempDir(), "no-such-lease-directory")

	t.Run("no context", func(t *testing.T) {
		_, _, err := heartbeatBoardLease(nil, stubBoardHeartbeater{t: t}, unreadable, "ra8-01")
		if err == nil {
			t.Fatal("a heartbeat without a context was accepted")
		}
		if err.Error() != "board heartbeat requires context and client" {
			t.Fatalf("refusal = %q, want the incomplete invocation", err)
		}
	})

	t.Run("no client", func(t *testing.T) {
		_, _, err := heartbeatBoardLease(context.Background(), nil, unreadable, "ra8-01")
		if err == nil {
			t.Fatal("a heartbeat without a client was accepted")
		}
		if err.Error() != "board heartbeat requires context and client" {
			t.Fatalf("refusal = %q, want the incomplete invocation", err)
		}
	})
}
