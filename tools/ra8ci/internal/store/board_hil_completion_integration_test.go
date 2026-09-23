//go:build integration

package store

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

type completionTestCatalog struct {
	digest string
	task   catalog.Task
}

func (c completionTestCatalog) Digest() string { return c.digest }

func (c completionTestCatalog) Task(name string) (catalog.Task, bool) {
	if name != c.task.Name {
		return catalog.Task{}, false
	}
	return c.task, true
}

func TestIntegrationCompleteBoardHILAttemptAndIdempotentReplay(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "hil-complete-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("c", 64)
	commitSHA := strings.Repeat("b", 40)
	now := time.Now().UTC()
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "integration HIL completion", Duration: 2 * time.Minute}
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
	task := catalog.Task{Name: "hil-integration", Version: 1, Tier: "required", Scope: "hil",
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
	attempt, err := s.StartBoardHILAttempt(ctx, boardAgent, run.Tasks[0].ID, waiter.LeaseID, testStart(run.Tasks[0].ID))
	if err != nil {
		t.Fatal(err)
	}
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}
	zero := 0
	steps := make([]HILStep, 0, len(task.Steps))
	for _, definition := range task.Steps {
		segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attempt.ID,
			definition.Name, 10*time.Second, 0)
		if err != nil {
			t.Fatalf("begin %s segment: %v", definition.Name, err)
		}
		step := HILStep{Key: definition.Name, StartedAt: segment.StartedAt,
			EndedAt: segment.StartedAt.Add(5 * time.Millisecond), DurationNS: (5 * time.Millisecond).Nanoseconds(),
			State: "succeeded", ChildExitCode: &zero}
		time.Sleep(10 * time.Millisecond)
		if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attempt.ID, "completed"); err != nil {
			t.Fatalf("finish %s segment: %v", definition.Name, err)
		}
		steps = append(steps, step)
	}
	completion := BoardHILCompletion{AttemptID: attempt.ID, LeaseID: waiter.LeaseID,
		Generation: active.Generation, Result: "succeeded", ChildExitCode: &zero,
		EvidenceComplete: true, Steps: steps}
	definitions := completionTestCatalog{digest: digest, task: task}
	if err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, commitSHA); err != nil {
		t.Fatalf("complete HIL attempt: %v", err)
	}
	if err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, commitSHA); err != nil {
		t.Fatalf("idempotent HIL completion retry: %v", err)
	}
	var attemptState, taskState string
	var stepCount int
	if err := pool.QueryRow(ctx, `SELECT a.state,t.state,(SELECT count(*) FROM task_steps WHERE attempt_id=a.id)
		FROM task_attempts a JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`, attempt.ID).
		Scan(&attemptState, &taskState, &stepCount); err != nil {
		t.Fatal(err)
	}
	if attemptState != "succeeded" || taskState != "succeeded" || stepCount != len(task.Steps) {
		t.Fatalf("HIL completion not atomically persisted: attempt=%s task=%s steps=%d", attemptState, taskState, stepCount)
	}
	var completionAudits int
	if err := pool.QueryRow(ctx, "SELECT count(*) FROM audit WHERE action=$1 AND target_id=$2", "task.attempt.finished", run.Tasks[0].ID).
		Scan(&completionAudits); err != nil {
		t.Fatal(err)
	}
	if completionAudits != 1 {
		t.Fatalf("completion retry duplicated terminal audit: %d events", completionAudits)
	}
}
