//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The board snapshot a read will not invent.
//
// Every board decision is taken against the reducer snapshot, so a damaged
// one is worse than a missing one: a board read as ready when it is held,
// or held by a lease that is not there, hands the hardware to two owners
// at once. Both doors into that row, the plain read and the command path
// that takes it FOR UPDATE, have to refuse the same damage rather than
// repair it or fill in a default, and neither may invent a board that was
// half created.
func TestIntegrationTheBoardSnapshotAReadWillNotInvent(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "snapshot-read-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	now := time.Now().UTC()

	// Nothing exists for this board yet: the registration above names an
	// actor, not hardware. The first command is what brings the typed row
	// and the reducer snapshot into being together, so that bootstrap is
	// the first thing worth pinning.
	if _, err := s.GetBoard(ctx, boardID); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an untouched board was not reported missing: %v", err)
	}
	first := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "snapshot bootstrap", Duration: time.Minute}
	if _, _, err := s.ApplyBoardCommand(ctx, holder, board.Enqueue{Waiter: first},
		0, nil, nil, now); err != nil {
		t.Fatalf("the first command did not create the board: %v", err)
	}
	var typed int
	if err := pool.QueryRow(ctx, "SELECT count(*) FROM boards WHERE id=$1", boardID).Scan(&typed); err != nil {
		t.Fatal(err)
	}
	if typed != 1 {
		t.Fatalf("the bootstrap left %d typed board rows", typed)
	}

	var version int64
	var raw []byte
	if err := pool.QueryRow(ctx, "SELECT version,state FROM board_snapshots WHERE board_id=$1",
		boardID).Scan(&version, &raw); err != nil {
		t.Fatalf("the bootstrap left no reducer snapshot: %v", err)
	}
	var sound board.Snapshot
	if err := json.Unmarshal(raw, &sound); err != nil {
		t.Fatal(err)
	}

	restore := func(t *testing.T) {
		t.Helper()
		if _, err := pool.Exec(ctx, `UPDATE board_snapshots SET version=$2,state=$3
			WHERE board_id=$1`, boardID, version, raw); err != nil {
			t.Fatal(err)
		}
	}
	// A command that is sound in every respect except the snapshot it has
	// to be applied to, so each case below says something about the row
	// rather than about the request.
	enqueue := func() error {
		waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
			Class: board.ClassAI, Reason: "snapshot damage proof", Duration: time.Minute}
		_, _, err := s.ApplyBoardCommand(ctx, holder, board.Enqueue{Waiter: waiter},
			uint64(version), nil, nil, now)
		return err
	}
	damage := func(t *testing.T, mutate func(*board.Snapshot)) {
		t.Helper()
		spoiled := sound
		mutate(&spoiled)
		encoded, err := json.Marshal(spoiled)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := pool.Exec(ctx, "UPDATE board_snapshots SET state=$2 WHERE board_id=$1",
			boardID, encoded); err != nil {
			t.Fatal(err)
		}
	}

	t.Run("a board ID the store will not take", func(t *testing.T) {
		_, err := s.GetBoard(ctx, " "+boardID)
		if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "board ID") {
			t.Fatalf("a padded board ID was accepted: %v", err)
		}
	})

	t.Run("a snapshot naming another board", func(t *testing.T) {
		damage(t, func(spoiled *board.Snapshot) { spoiled.BoardID = "somewhere-else" })
		defer restore(t)
		if _, err := s.GetBoard(ctx, boardID); !errors.Is(err, ErrUnavailable) ||
			!strings.Contains(err.Error(), "snapshot is corrupt") {
			t.Fatalf("the read accepted another board's state: %v", err)
		}
		if err := enqueue(); !errors.Is(err, ErrUnavailable) ||
			!strings.Contains(err.Error(), "snapshot is corrupt") {
			t.Fatalf("the command path accepted another board's state: %v", err)
		}
	})

	t.Run("a version the snapshot does not agree with", func(t *testing.T) {
		// The row's own version column and the version inside the encoded
		// state are two records of the same fact. When they disagree there
		// is no way to tell which write was lost, so neither is believed.
		if _, err := pool.Exec(ctx, "UPDATE board_snapshots SET version=version+5 WHERE board_id=$1",
			boardID); err != nil {
			t.Fatal(err)
		}
		defer restore(t)
		if _, err := s.GetBoard(ctx, boardID); !errors.Is(err, ErrUnavailable) ||
			!strings.Contains(err.Error(), "snapshot is corrupt") {
			t.Fatalf("the read papered over a version disagreement: %v", err)
		}
	})

	t.Run("a live phase with no lease behind it", func(t *testing.T) {
		// Well-formed, correctly addressed, correctly versioned, and still
		// a lie: an active board with no lease would read as hardware
		// someone holds, with nobody to ask for it back.
		damage(t, func(spoiled *board.Snapshot) {
			spoiled.Phase = board.Active
			spoiled.Lease = nil
		})
		defer restore(t)
		if _, err := s.GetBoard(ctx, boardID); !errors.Is(err, ErrUnavailable) ||
			!strings.Contains(err.Error(), "snapshot is invalid") {
			t.Fatalf("the read accepted an active board with no lease: %v", err)
		}
		if err := enqueue(); !errors.Is(err, ErrUnavailable) ||
			!strings.Contains(err.Error(), "snapshot is invalid") {
			t.Fatalf("the command path accepted an active board with no lease: %v", err)
		}
	})

	t.Run("a typed board with no reducer snapshot", func(t *testing.T) {
		// Half a board: the typed row exists, the reducer state does not.
		// Creating a fresh snapshot here would silently release a board
		// that may well be held, so the command path refuses instead.
		// It gets its own board, because once a board has events its
		// snapshot row cannot be removed to simulate this.
		half := "snapshot-half-" + mustID(t)
		halfHolder := boardTestActor(t, ctx, s, pool, half, "agent", "submitter")
		if _, err := pool.Exec(ctx, `INSERT INTO boards (id,generation,state,version)
			VALUES ($1,0,'available',0)`, half); err != nil {
			t.Fatal(err)
		}
		if _, err := s.GetBoard(ctx, half); !errors.Is(err, ErrNotFound) {
			t.Fatalf("the read invented a snapshot: %v", err)
		}
		waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: halfHolder.ID(),
			Class: board.ClassAI, Reason: "half board proof", Duration: time.Minute}
		_, _, err := s.ApplyBoardCommand(ctx, halfHolder, board.Enqueue{Waiter: waiter},
			0, nil, nil, now)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "without reducer snapshot") {
			t.Fatalf("the command path rebuilt a half-created board: %v", err)
		}
	})

	t.Run("the snapshot as the fixture wrote it", func(t *testing.T) {
		// The control: with the row back as it was, the same read returns
		// the board and the same command is accepted, which is what keeps
		// the refusals above from passing for some unrelated reason.
		snapshot, err := s.GetBoard(ctx, boardID)
		if err != nil {
			t.Fatalf("a sound snapshot was refused: %v", err)
		}
		if snapshot.BoardID != boardID || snapshot.Version != uint64(version) ||
			snapshot.Phase != sound.Phase {
			t.Fatalf("the read returned a different board: %+v", snapshot)
		}
		if err := enqueue(); err != nil {
			t.Fatalf("a sound command was refused: %v", err)
		}
	})
}
