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

type queueTestCatalog struct {
	digest string
	task   catalog.Task
}

func (c queueTestCatalog) Digest() string { return c.digest }

func (c queueTestCatalog) Task(name string) (catalog.Task, bool) {
	if name != c.task.Name {
		return catalog.Task{}, false
	}
	return c.task, true
}

// The queue a board agent is offered, and the work it is not.
//
// A board agent asks the plane for its next hardware task under a live
// lease, and the plane answers from the reviewed catalog and the commit
// the agent says it is running. Work planned under a different catalog
// or a different commit is not merely skipped by accident: it must be
// invisible, because handing it over would run hardware against a
// contract nobody reviewed. Where a task IS selected and then fails to
// match, that is a conflict the agent has to see rather than a quiet
// empty queue.
func TestIntegrationTheQueueABoardAgentIsOffered(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	boardID := "hil-queue-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("e", 64)
	commitSHA := strings.Repeat("f", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "a queue under test", Duration: 2 * time.Minute}
	pending, _, err := s.ApplyBoardCommand(ctx, holder, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.ApplyBoardCommand(ctx, boardAgent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: pending.Generation, InstalledGeneration: pending.Generation,
	}, pending.Version, nil, nil, now.Add(time.Second)); err != nil {
		t.Fatal(err)
	}

	hil := &catalog.HILTask{BoardID: boardID, BoardModel: "EK-RA8D2",
		ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily: "uart-demo", Mode: "uart_scrape", ObservationStep: "observe",
		FlashRestoreSeconds: 10}
	task := catalog.Task{Name: "hil-queued", Version: 1, Tier: "required", Scope: "hil",
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
		Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: commitSHA,
		SnapshotSHA256: strings.Repeat("d", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: json.RawMessage(arguments),
			Tier: task.Tier, Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds}}})
	if err != nil {
		t.Fatal(err)
	}
	reviewed := queueTestCatalog{digest: digest, task: task}
	start := func() StartAttemptInput { return testStart(run.Tasks[0].ID) }

	t.Run("work planned under another commit is not offered", func(t *testing.T) {
		assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			reviewed, strings.Repeat("a", 40))
		if err != nil || assignment != nil {
			t.Fatalf("a task from another commit was offered: %+v err=%v", assignment, err)
		}
	})

	t.Run("work planned under another catalog is not offered", func(t *testing.T) {
		stale := queueTestCatalog{digest: strings.Repeat("9", 64), task: task}
		assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			stale, commitSHA)
		if err != nil || assignment != nil {
			t.Fatalf("a task from another catalog was offered: %+v err=%v", assignment, err)
		}
	})

	t.Run("a selected task the catalog no longer carries", func(t *testing.T) {
		renamed := task
		renamed.Name = "hil-renamed"
		assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			queueTestCatalog{digest: digest, task: renamed}, commitSHA)
		if assignment != nil || !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "differs from the current reviewed catalog") {
			t.Fatalf("a task missing from the catalog was handed over: %+v err=%v", assignment, err)
		}
	})

	t.Run("a selected task whose catalog deadline has moved", func(t *testing.T) {
		shortened := task
		shortened.DeadlineSeconds = task.DeadlineSeconds - 1
		assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			queueTestCatalog{digest: digest, task: shortened}, commitSHA)
		if assignment != nil || !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "differs from the current reviewed catalog") {
			t.Fatalf("a task whose deadline moved was handed over: %+v err=%v", assignment, err)
		}
	})

	t.Run("a selected task bound to another board", func(t *testing.T) {
		elsewhere := *hil
		elsewhere.BoardID = "hil-queue-" + mustID(t)
		moved := task
		moved.HIL = &elsewhere
		assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			queueTestCatalog{digest: digest, task: moved}, commitSHA)
		if assignment != nil || !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "differs from the current reviewed catalog") {
			t.Fatalf("another board's task was handed over: %+v err=%v", assignment, err)
		}
	})

	t.Run("nothing was claimed while the queue was refusing", func(t *testing.T) {
		var attempts int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM task_attempts
			WHERE board_lease_id=$1`, waiter.LeaseID).Scan(&attempts); err != nil {
			t.Fatal(err)
		}
		if attempts != 0 {
			t.Fatalf("the refusals left %d attempts behind", attempts)
		}
	})

	t.Run("the work the reviewed catalog does describe", func(t *testing.T) {
		assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			reviewed, commitSHA)
		if err != nil || assignment == nil {
			t.Fatalf("the reviewed task was not offered: %v", err)
		}
		if assignment.Attempt.TaskID != run.Tasks[0].ID || assignment.CatalogSHA256 != digest ||
			assignment.CommitSHA != commitSHA || assignment.Task.Name != task.Name {
			t.Fatalf("the assignment did not describe the reviewed work: %+v", assignment)
		}
		// The timing is pinned by the plane and read back out of the
		// audit trail, so the agent runs the window the server chose.
		if assignment.HILTiming == nil || assignment.HILTiming.Decision.ValidityWindow <= 0 ||
			assignment.HILTiming.Decision.FlashRestoreBound != 10*time.Second {
			t.Fatalf("the assignment carried no pinned timing: %+v", assignment.HILTiming)
		}
		// The persisted argv is carried through as it was stored, and
		// this task stores none, so an empty argument list is what the
		// agent must be handed rather than anything invented here.
		if len(assignment.Args) != 0 {
			t.Fatalf("the assignment invented arguments: %+v", assignment.Args)
		}
	})
}
