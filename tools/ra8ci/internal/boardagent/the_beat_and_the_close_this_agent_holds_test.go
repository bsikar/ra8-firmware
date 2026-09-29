// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestReportAliveRefusesAnAnswerAboutAnotherBoard(t *testing.T) {
	// A beat is one answer about one board. A snapshot carrying a different
	// board is not this holder's liveness, however healthy it looks, so it
	// is refused before either half of the answer is read.
	agent, client, token := newBeatingAgent(t)
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		elsewhere := client.state
		elsewhere.BoardID = "ek-ra8m1"
		return elsewhere, boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID, Beat: true}, nil
	}
	liveness, err := agent.ReportAlive(context.Background(), token)
	if !errors.Is(err, boardclient.ErrInvalidRequest) {
		t.Fatalf("error = %v", err)
	}
	if liveness.Held || liveness.LeaseID != "" {
		t.Fatalf("a board that is not ours reported liveness: %+v", liveness)
	}
}

func TestKeepAliveRefusesATokenItCouldNeverBeatFor(t *testing.T) {
	agent, client, _ := newBeatingAgent(t)
	if err := agent.KeepAlive(context.Background(), boardclient.LeaseToken{}); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("error = %v", err)
	}
	if client.count() != 0 {
		t.Fatalf("a token that authorizes nothing still beat %d times", client.count())
	}
}

func TestKeepAliveEndsQuietlyWhenTheHoldersWorkWasCancelled(t *testing.T) {
	// A cancelled context is the normal end of a holder's work, so the beat
	// that was in flight when it ended is not an error to report. The refusal
	// reaching the loop is the context's, not the server's.
	agent, client, token := newBeatingAgent(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		cancel()
		return board.Snapshot{}, boardclient.HolderLiveness{}, errors.New("beat never reached the server")
	}
	if err := agent.KeepAlive(ctx, token); err != nil {
		t.Fatalf("a cancelled holder ended with an error: %v", err)
	}
	if client.count() != 1 {
		t.Fatalf("beats = %d, want the one that was in flight", client.count())
	}
}

func TestCompleteHILAttemptRefusesWhileTheLocalGateIsHeld(t *testing.T) {
	// Terminal persistence is allowed after a yield request, but it still
	// waits for the local gate: it must not run beside the hardware work it
	// is reporting on.
	agent, client, token := newActiveSegmentAgent(t)
	task := catalog.Task{Name: "hil-test", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 30, BoardPolicy: "exclusive",
		Steps: []catalog.Step{{Name: "observe", Program: "fixture"}},
		Retry: catalog.RetryPolicy{MaxAttempts: 1},
		HIL: &catalog.HILTask{BoardID: token.BoardID, BoardModel: "EK-RA8D2",
			ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
			ProgramFamily: "demo", Mode: "alive", ObservationStep: "observe", FlashRestoreSeconds: 5}}
	if err := catalog.ValidateTask(task); err != nil {
		t.Fatalf("test task invalid: %v", err)
	}
	assignment := store.BoardHILAssignment{Attempt: store.Attempt{
		ID: "01996f90-3415-7cfe-8ff1-600058131aff", State: "running"}, Task: task}
	completion := store.BoardHILCompletion{AttemptID: assignment.Attempt.ID, LeaseID: token.LeaseID,
		Generation: token.Generation, Result: "preempted", EvidenceComplete: false}
	holdTheGate(t, agent)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := agent.CompleteHILAttempt(ctx, token, assignment, completion); !errors.Is(err, context.Canceled) {
		t.Fatalf("error = %v", err)
	}
	if client.completed != 0 {
		t.Fatalf("a held gate still persisted %d completions", client.completed)
	}
}
