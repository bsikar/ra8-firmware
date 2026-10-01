//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The segments a board will not open.
//
// A bounded segment is the window in which the hardware is allowed to be in
// an unknown state, so opening one is the plane's promise that the board can
// be brought back before anything else needs it. Every refusal here protects
// that promise from a different direction: a window longer than the lease,
// longer than the attempt it belongs to, opened by the wrong hand, or opened
// on top of one that is still running. Each refused open has to leave no
// segment behind at all, because a phantom open segment blocks the board for
// everyone until someone reconciles it by hand.
func TestIntegrationTheSegmentsABoardWillNotOpen(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	boardID := "seg-open-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("9", 64)
	commit := strings.Repeat("7", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "segment opening proof", Duration: 3 * time.Minute}
	pending, _, err := s.ApplyBoardCommand(ctx, holder, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, boardAgent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: pending.Generation, InstalledGeneration: pending.Generation,
	}, pending.Version, nil, nil, now.Add(time.Second))
	if err != nil || active.Phase != board.Active {
		t.Fatalf("board lease did not activate: phase=%s err=%v", active.Phase, err)
	}

	hil := &catalog.HILTask{BoardID: boardID, BoardModel: "EK-RA8D2",
		ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily: "uart-demo", Mode: "uart_scrape", ObservationStep: "observe",
		FlashRestoreSeconds: 10}
	task := catalog.Task{Name: "seg-open", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "exclusive", HIL: hil,
		Steps: []catalog.Step{{Name: "flash", Program: "test-adapter"}, {Name: "observe", Program: "test-adapter"}},
		Retry: catalog.RetryPolicy{MaxAttempts: 1}}
	if err := catalog.ValidateTask(task); err != nil {
		t.Fatalf("invalid test HIL task: %v", err)
	}
	encodedHIL, err := json.Marshal(hil)
	if err != nil {
		t.Fatal(err)
	}
	arguments := append([]byte(`{"argv":[],"hil":`), encodedHIL...)
	arguments = append(arguments, byte(125))
	run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: holder.ID(),
		Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: commit,
		SnapshotSHA256: strings.Repeat("8", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: json.RawMessage(arguments),
			Tier: task.Tier, Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds}}})
	if err != nil {
		t.Fatal(err)
	}
	definitions := completionTestCatalog{digest: digest, task: task}
	assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID,
		testStart(run.Tasks[0].ID), definitions, commit)
	if err != nil || assignment == nil {
		t.Fatalf("claim HIL assignment: %v", err)
	}
	attemptID := assignment.Attempt.ID
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}

	noSegment := func(t *testing.T) {
		t.Helper()
		var segments int
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM board_segments WHERE board_id=$1",
			boardID).Scan(&segments); err != nil {
			t.Fatal(err)
		}
		if segments != 0 {
			t.Fatalf("a refused open left %d segment(s) on the board", segments)
		}
	}

	t.Run("a generation no column could hold", func(t *testing.T) {
		wide := token
		wide.Generation = math.MaxInt64 + 1
		_, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, wide, attemptID,
			"flash", 10*time.Second, 0)
		if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "board segment generation") {
			t.Fatalf("an unstorable generation was accepted: %v", err)
		}
		noSegment(t)
	})

	t.Run("a board that moved since the caller read it", func(t *testing.T) {
		_, err := s.BeginBoardSegment(ctx, boardAgent, active.Version+1, token, attemptID,
			"flash", 10*time.Second, 0)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "stale board version") {
			t.Fatalf("a stale version was accepted: %v", err)
		}
		noSegment(t)
	})

	t.Run("a token naming a lease the board is not running", func(t *testing.T) {
		stale := board.Token{BoardID: boardID, LeaseID: mustID(t), Generation: active.Generation}
		_, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, stale, attemptID,
			"flash", 10*time.Second, 0)
		if err == nil || !strings.Contains(err.Error(), "does not match the active grant") {
			t.Fatalf("a token from another grant was accepted: %v", err)
		}
		noSegment(t)
	})

	t.Run("a window longer than the lease has left", func(t *testing.T) {
		// The board has to be restorable before the lease ends, so the
		// bound plus its recovery margin has to fit inside what remains.
		_, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
			"flash", time.Hour, 0)
		if err == nil || !strings.Contains(err.Error(), "insufficient lease time") {
			t.Fatalf("a window past the lease was accepted: %v", err)
		}
		noSegment(t)
	})

	t.Run("the lease holder rather than the board agent", func(t *testing.T) {
		// The holder owns the board and still may not open a segment:
		// only the physical agent is in a position to know the hardware
		// is in the state the segment claims it is.
		_, err := s.BeginBoardSegment(ctx, holder, active.Version, token, attemptID,
			"flash", 10*time.Second, 0)
		if !errors.Is(err, ErrDenied) {
			t.Fatalf("the lease holder opened a segment: %v", err)
		}
		noSegment(t)
	})

	t.Run("an attempt the lease is not running", func(t *testing.T) {
		_, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, mustID(t),
			"flash", 10*time.Second, 0)
		if err == nil || !strings.Contains(err.Error(), "not active under this board lease") {
			t.Fatalf("a segment was opened for an unrelated attempt: %v", err)
		}
		noSegment(t)
	})

	t.Run("a window longer than the attempt it belongs to", func(t *testing.T) {
		// The lease could carry this window; the attempt could not. A
		// segment outliving its attempt would hold the board for work
		// the plane has already given up on.
		_, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
			"flash", 90*time.Second, 0)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "deadline is too close") {
			t.Fatalf("a window past the attempt deadline was accepted: %v", err)
		}
		noSegment(t)
	})

	var opened BoardSegment
	t.Run("a window the lease and the attempt both carry", func(t *testing.T) {
		// The control: the same call the refusals above vary from one
		// field at a time, with every field sound, is accepted.
		opened, err = s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
			"flash", 10*time.Second, 2*time.Second)
		if err != nil {
			t.Fatalf("a sound segment was refused: %v", err)
		}
		var key string
		var margin int64
		var open bool
		if err := pool.QueryRow(ctx, `SELECT segment_key,recovery_margin_ms,ended_at IS NULL
			FROM board_segments WHERE id=$1`, opened.ID).Scan(&key, &margin, &open); err != nil {
			t.Fatal(err)
		}
		if key != "flash" || margin != 2000 || !open {
			t.Fatalf("the segment was not recorded as opened: key=%q margin=%d open=%v", key, margin, open)
		}
	})

	t.Run("a second window while the first is still open", func(t *testing.T) {
		_, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
			"observe", 10*time.Second, 0)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "prior segment is still open") {
			t.Fatalf("two segments were open on one board: %v", err)
		}
		var segments int
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM board_segments WHERE board_id=$1",
			boardID).Scan(&segments); err != nil {
			t.Fatal(err)
		}
		if segments != 1 {
			t.Fatalf("the refused second open left %d segments", segments)
		}
	})

	t.Run("the observation window once the first one closes", func(t *testing.T) {
		// And the refusal above was about overlap, not about the second
		// window itself: closing the first one lets it through.
		if err := s.FinishBoardSegment(ctx, boardAgent, opened.ID, token, attemptID, "completed"); err != nil {
			t.Fatalf("close the first segment: %v", err)
		}
		second, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
			"observe", 10*time.Second, 0)
		if err != nil {
			t.Fatalf("the second segment was refused after the first closed: %v", err)
		}
		if second.ID == opened.ID {
			t.Fatal("the second open returned the first segment")
		}
	})
}
