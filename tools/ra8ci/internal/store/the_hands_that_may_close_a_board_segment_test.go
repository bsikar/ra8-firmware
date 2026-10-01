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
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// Who may close a bounded board segment, and how often.
//
// Closing a segment is what tells the plane the hardware is back in a known
// state, so it is the agent's own claim about its own work. Every way of
// getting that claim wrong lands as the same denial, which is deliberate:
// the door does not explain to a caller which part of its token was wrong.
// So the assertion that carries weight here is not the message but the
// segment: after each refusal it has to still be open, because a segment
// wrongly closed is a board reported free while it is still in use.
func TestIntegrationTheHandsThatMayCloseABoardSegment(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "seg-close-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("c", 64)
	commit := strings.Repeat("b", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "segment closing proof", Duration: 2 * time.Minute}
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
	task := catalog.Task{Name: "seg-close", Version: 1, Tier: "required", Scope: "hil",
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
		SnapshotSHA256: strings.Repeat("d", 64), CatalogSHA256: digest,
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

	segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
		"flash", 10*time.Second, 0)
	if err != nil {
		t.Fatalf("begin flash segment: %v", err)
	}

	stillOpen := func(t *testing.T) {
		t.Helper()
		var open bool
		if err := pool.QueryRow(ctx, "SELECT ended_at IS NULL FROM board_segments WHERE id=$1",
			segment.ID).Scan(&open); err != nil {
			t.Fatal(err)
		}
		if !open {
			t.Fatal("a refused close ended the segment anyway")
		}
	}

	refusals := []struct {
		name    string
		actor   BoardActor
		segment string
		token   board.Token
		attempt string
	}{
		{"a segment that does not exist", boardAgent, mustID(t), token, attemptID},
		{"a token naming another lease", boardAgent, segment.ID,
			board.Token{BoardID: boardID, LeaseID: mustID(t), Generation: active.Generation}, attemptID},
		{"a token a generation ahead", boardAgent, segment.ID,
			board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation + 1}, attemptID},
		{"an attempt the segment was not opened for", boardAgent, segment.ID, token, mustID(t)},
	}
	for _, one := range refusals {
		t.Run(one.name, func(t *testing.T) {
			err := s.FinishBoardSegment(ctx, one.actor, one.segment, one.token, one.attempt, "completed")
			if !errors.Is(err, ErrDenied) {
				t.Fatalf("the close was not denied: %v", err)
			}
			stillOpen(t)
		})
	}

	t.Run("another agent on the same board", func(t *testing.T) {
		// Correct in everything the board can check about it: a real
		// board agent, on this board, holding a sound token. It simply
		// is not the agent that opened the segment, and only that agent
		// knows whether the work inside it finished.
		stranger := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
		err := s.FinishBoardSegment(ctx, stranger, segment.ID, token, attemptID, "completed")
		if !errors.Is(err, ErrDenied) {
			t.Fatalf("another agent closed a segment it did not open: %v", err)
		}
		stillOpen(t)
	})

	t.Run("the agent that opened it", func(t *testing.T) {
		if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attemptID, "completed"); err != nil {
			t.Fatalf("the owning agent could not close its own segment: %v", err)
		}
		var outcome string
		var closed bool
		if err := pool.QueryRow(ctx, `SELECT outcome, ended_at IS NOT NULL
			FROM board_segments WHERE id=$1`, segment.ID).Scan(&outcome, &closed); err != nil {
			t.Fatal(err)
		}
		if outcome != "completed" || !closed {
			t.Fatalf("the segment did not close: outcome=%q closed=%v", outcome, closed)
		}
	})

	t.Run("the same agent closing it twice", func(t *testing.T) {
		// A repeat is not free here, unlike a repeated completion: the
		// second close would move the outcome of a segment the plane has
		// already read, so it is refused rather than absorbed.
		err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attemptID, "failed")
		if !errors.Is(err, ErrDenied) {
			t.Fatalf("a closed segment was closed again: %v", err)
		}
		var outcome string
		if err := pool.QueryRow(ctx, "SELECT outcome FROM board_segments WHERE id=$1",
			segment.ID).Scan(&outcome); err != nil {
			t.Fatal(err)
		}
		if outcome != "completed" {
			t.Fatalf("the second close rewrote the outcome: %q", outcome)
		}
	})
}
