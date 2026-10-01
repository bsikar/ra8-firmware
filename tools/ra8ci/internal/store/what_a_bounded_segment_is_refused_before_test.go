//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The two refusals a bounded segment meets after its arguments are read
// and before the board is touched.
//
// Both are reached with nothing but a live board: no run, no HIL task and
// no attempt, because each refusal is decided ahead of the attempt being
// looked at. The happy path is already pinned in board_integration_test.go,
// so what is pinned here is that a refusal leaves the board exactly as it
// found it, which is the part a caller cannot see from the error alone.
func TestIntegrationWhatABoundedSegmentIsRefusedBefore(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	s, pool, human, agent, active, waiter, _ := activeTestBoard(t, ctx)
	boardID := active.BoardID
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}

	// An identifier of the right shape for an attempt that does not exist.
	// Every refusal below is decided before anything reads it.
	unread := mustID(t)

	unchanged := func(t *testing.T, why string) {
		t.Helper()
		after, err := s.GetBoard(ctx, boardID)
		if err != nil {
			t.Fatal(err)
		}
		if after.Version != active.Version || after.Phase != active.Phase {
			t.Fatalf("%s moved the board: version %d -> %d, phase %s -> %s",
				why, active.Version, after.Version, active.Phase, after.Phase)
		}
	}

	t.Run("a board that has moved since the caller read it", func(t *testing.T) {
		// The agent holds the board and is asking for sound work. What it
		// is working from is a version that is no longer current, so the
		// plane cannot know the segment it is about to start is the one
		// the caller still means.
		_, err := s.BeginBoardSegment(ctx, agent, active.Version+1, token, unread, "flash", 20*time.Second, 3*time.Second)
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("a stale board version was not a conflict: %v", err)
		}
		unchanged(t, "a stale version")
	})

	t.Run("a caller holding no lease on the board", func(t *testing.T) {
		// A second human on the same board, correct in every argument and
		// reading the current version, but holding nothing. Only the
		// holder and a physical board agent may bound the hardware.
		stranger := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
		_, err := s.BeginBoardSegment(ctx, stranger, active.Version, token, unread, "flash", 20*time.Second, 3*time.Second)
		if !errors.Is(err, ErrDenied) {
			t.Fatalf("a stranger bounded the board: %v", err)
		}
		unchanged(t, "a denied caller")
	})

	t.Run("the holder is not denied on the same arguments", func(t *testing.T) {
		// The anti-vacuity case. The holder's own agent passes both
		// checks above and is judged on what it asked for instead: the
		// attempt it names does not exist. Anything is acceptable here
		// except the two refusals under test, which would mean they are
		// refusing every caller rather than the ones named above.
		_, err := s.BeginBoardSegment(ctx, agent, active.Version, token, unread, "flash", 20*time.Second, 3*time.Second)
		if errors.Is(err, ErrDenied) {
			t.Fatalf("the holder was denied its own board: %v", err)
		}
		if err == nil {
			t.Fatal("a segment started against an attempt that does not exist")
		}
		unchanged(t, "an unknown attempt")
	})

	// The human holder is the one activeTestBoard granted the lease to;
	// keeping it referenced documents that the stranger above is a
	// genuinely different principal on the same board.
	if human.ID() == "" {
		t.Fatal("the fixture holder has no identity")
	}
}
