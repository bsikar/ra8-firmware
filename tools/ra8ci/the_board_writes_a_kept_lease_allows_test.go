// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// The board write commands read a lease token this machine kept, ask the plane
// what the board looks like now, and only then write. That first read is the
// safety property: a token this machine still holds says nothing about whether
// the board is still held by it. These drive the commands end to end, over a
// real client against a stand-in plane, with a token actually on disk.

// heldBoard points the configuration home at a private directory, writes a
// lease token there, and returns the token the commands will read back.
func heldBoard(t *testing.T) boardclient.LeaseToken {
	t.Helper()
	home := filepath.Join(t.TempDir(), "config")
	if err := os.Mkdir(home, 0o700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("XDG_CONFIG_HOME", home)
	directory, err := currentBoardLeaseDirectory()
	if err != nil {
		t.Fatalf("the lease directory was unavailable: %v", err)
	}
	token := soundLeaseToken(t)
	if err := writeBoardLeaseToken(directory, token); err != nil {
		t.Fatalf("the lease token could not be kept: %v", err)
	}
	return token
}

// answeringStatus serves one board status and 404s everything else, so a
// command that tries to write is refused by the stand-in rather than quietly
// succeeding against a path nobody served.
func answeringStatus(t *testing.T, snapshot board.Snapshot) http.HandlerFunc {
	t.Helper()
	return atPath(t, "/v1/boards/"+snapshot.BoardID, snapshot)
}

// A board the plane reports as ready is a board nobody holds. Every write
// command stops there, because the alternative is writing under a lease that
// has already been handed to somebody else.
func TestTheBoardWritesStopWhenThePlaneReportsNoSuchLease(t *testing.T) {
	for _, command := range []struct {
		name  string
		reads string
		run   func(context.Context, string) error
	}{
		{"checkpoint", "board checkpoint", func(ctx context.Context, id string) error {
			return boardCheckpointCommand(ctx, []string{id})
		}},
		{"extend", "extend board lease", func(ctx context.Context, id string) error {
			return boardExtendCommand(ctx, []string{id, "--why", "still bringing up the bench", "--duration", "1h"})
		}},
	} {
		t.Run(command.name, func(t *testing.T) {
			token := heldBoard(t)
			servingBoard(t, answeringStatus(t, readyBoard(token.BoardID)))

			spoke, err := spoken(t, func() error {
				return command.run(context.Background(), token.BoardID)
			})
			if err == nil {
				t.Fatalf("a write under a lease the plane does not record was accepted: %q", spoke)
			}
			if !strings.HasPrefix(err.Error(), command.reads) {
				t.Errorf("the operator read %q", err)
			}
			if spoke != "" {
				t.Errorf("a refused write still spoke %q", spoke)
			}
		})
	}
}

// Heartbeat says it in the operator's terms rather than handing back the
// library's words: a beat that finds the board held by somebody else is the one
// refusal an operator has to understand immediately.
func TestABeatOnALeaseTheBoardNoLongerHoldsSaysSoPlainly(t *testing.T) {
	token := heldBoard(t)
	servingBoard(t, answeringStatus(t, readyBoard(token.BoardID)))

	spoke, err := spoken(t, func() error {
		return boardHeartbeatCommand(context.Background(), []string{token.BoardID})
	})
	if err == nil {
		t.Fatalf("a beat on a lease the board does not hold was accepted: %q", spoke)
	}
	if err.Error() != "board "+token.BoardID+" is no longer held by this lease" {
		t.Errorf("the operator read %q", err)
	}
	if spoke != "" {
		t.Errorf("a refused beat still spoke %q", spoke)
	}
}

// The lease store is read before the plane is asked anything, so a machine
// holding no token for this board never spends a request.
func TestAWriteWithNoKeptTokenNeverAsksThePlane(t *testing.T) {
	asked := false
	token := heldBoard(t)
	servingBoard(t, func(writer http.ResponseWriter, request *http.Request) {
		asked = true
		writer.WriteHeader(http.StatusNotFound)
	})

	other := token.BoardID + "-spare"
	if err := boardHeartbeatCommand(context.Background(), []string{other}); err == nil {
		t.Fatal("a beat with no kept token was accepted")
	}
	if asked {
		t.Error("the plane was asked about a board this machine holds no token for")
	}
}

// An extension the plane could not be asked about is reported as an extension,
// not as a lease problem: the token was found and believed, and what failed was
// the exchange.
func TestAnExtensionThePlaneWillNotAnswerIsReportedAsAnExtension(t *testing.T) {
	token := heldBoard(t)
	servingBoard(t, func(writer http.ResponseWriter, request *http.Request) {
		writer.WriteHeader(http.StatusServiceUnavailable)
	})

	spoke, err := spoken(t, func() error {
		return boardExtendCommand(context.Background(),
			[]string{token.BoardID, "--why", "flashing the bench", "--duration", "30m"})
	})
	if err == nil {
		t.Fatalf("an unanswered extension was accepted: %q", spoke)
	}
	if !strings.HasPrefix(err.Error(), "extend board lease") {
		t.Errorf("the operator read %q", err)
	}
}
