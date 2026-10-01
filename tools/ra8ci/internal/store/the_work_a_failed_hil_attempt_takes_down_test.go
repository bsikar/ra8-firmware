//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

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

// What a failed HIL attempt takes down with it.
//
// Lab work sits at the head of pipelines: the board proves the firmware
// runs, and everything downstream assumes it did. So a HIL failure has to
// travel the dependency edges exactly as a local failure does, all the way
// to the far end of the chain, or a pipeline quietly strands tasks that can
// never become runnable. The board path reaches that sweep through its own
// completion door, with a bounded segment and a signed claim in the way,
// which is why it is worth pinning separately from the local path.
func TestIntegrationTheWorkAFailedHILAttemptTakesDown(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	boardID := "hil-skip-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("1", 64)
	commit := strings.Repeat("2", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "dependent skip proof", Duration: 3 * time.Minute}
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
	task := catalog.Task{Name: "hil-head", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 90, BoardPolicy: "exclusive", HIL: hil,
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

	// The shape under test: the lab task at the head, two tasks chained
	// behind it so the sweep has to travel two edges, and one task beside
	// the chain that depends on nothing.
	local := func(key string, dependsOn ...string) TaskInput {
		return TaskInput{Key: key, Name: "format-check", Arguments: json.RawMessage(`{"argv":[]}`),
			DependsOnKeys: dependsOn, Tier: "required", Scope: "safe-local-read-only",
			HostClass: "linux-vm", DeadlineSeconds: 60}
	}
	run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: holder.ID(),
		Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: commit,
		SnapshotSHA256: strings.Repeat("3", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{
			{Key: "hil", Name: task.Name, Arguments: json.RawMessage(arguments), Tier: task.Tier,
				Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds},
			local("after", "hil"),
			local("later", "after"),
			local("beside"),
		}})
	if err != nil {
		t.Fatal(err)
	}
	ids := map[string]string{}
	for _, one := range run.Tasks {
		ids[one.Key] = one.ID
	}
	if len(ids) != 4 {
		t.Fatalf("the run did not carry four tasks: %v", ids)
	}

	definitions := completionTestCatalog{digest: digest, task: task}
	assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID,
		testStart(ids["hil"]), definitions, commit)
	if err != nil || assignment == nil {
		t.Fatalf("claim HIL assignment: %v", err)
	}
	if assignment.Attempt.TaskID != ids["hil"] {
		t.Fatalf("the claim took a different task: %s", assignment.Attempt.TaskID)
	}
	attemptID := assignment.Attempt.ID
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}

	// The flash step broke, so its segment closes as failed and the report
	// carries that one step. A failure may be partial, because work stops
	// where it broke; the observation step never ran.
	segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
		"flash", 10*time.Second, 0)
	if err != nil {
		t.Fatalf("begin flash segment: %v", err)
	}
	if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attemptID, "failed"); err != nil {
		t.Fatalf("finish flash segment: %v", err)
	}
	var started, ended time.Time
	if err := pool.QueryRow(ctx, "SELECT started_at,ended_at FROM board_segments WHERE id=$1",
		segment.ID).Scan(&started, &ended); err != nil {
		t.Fatal(err)
	}
	one := 1
	completion := BoardHILCompletion{AttemptID: attemptID, LeaseID: waiter.LeaseID,
		Generation: active.Generation, Result: "failed", ChildExitCode: &one,
		Reason: "flash_failed",
		Steps: []HILStep{{Key: "flash", StartedAt: started, EndedAt: ended,
			DurationNS: ended.Sub(started).Nanoseconds(), State: "failed", ChildExitCode: &one}}}
	if err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, commit); err != nil {
		t.Fatalf("complete the failed HIL attempt: %v", err)
	}

	states := map[string]string{}
	reasons := map[string]string{}
	rows, err := pool.Query(ctx, `SELECT task_key,state,coalesce(skip_reason,'')
		FROM tasks WHERE run_id=$1`, run.ID)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		var key, state, reason string
		if err := rows.Scan(&key, &state, &reason); err != nil {
			rows.Close()
			t.Fatal(err)
		}
		states[key] = state
		reasons[key] = reason
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}

	if states["hil"] != "failed" {
		t.Fatalf("the lab task did not fail: %s", states["hil"])
	}
	if states["after"] != "skipped" || states["later"] != "skipped" {
		t.Fatalf("the failure did not travel the chain: after=%s later=%s",
			states["after"], states["later"])
	}
	if reasons["after"] != "prerequisite_failed" || reasons["later"] != "prerequisite_failed" {
		t.Fatalf("the skips carry the wrong reason: after=%q later=%q",
			reasons["after"], reasons["later"])
	}
	// The control, and the whole point of naming the edges: a task beside
	// the chain is still runnable, so a sweep that took it too would be
	// cancelling work the failure says nothing about.
	if states["beside"] != "scheduled" {
		t.Fatalf("an independent task was swept up: %s", states["beside"])
	}

	var attemptState string
	if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1",
		attemptID).Scan(&attemptState); err != nil {
		t.Fatal(err)
	}
	if attemptState != "failed" {
		t.Fatalf("the attempt did not finish as failed: %s", attemptState)
	}

	var skipped int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM run_events
		WHERE run_id=$1 AND kind='task.skipped'`, run.ID).Scan(&skipped); err != nil {
		t.Fatal(err)
	}
	if skipped != 2 {
		t.Fatalf("the run carries %d task.skipped events, want 2", skipped)
	}
	var audited int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM audit
		WHERE correlation_run_id=$1 AND action='task.skipped' AND new_state='skipped'`,
		run.ID).Scan(&audited); err != nil {
		t.Fatal(err)
	}
	if audited != 2 {
		t.Fatalf("the skips left %d audit rows, want 2", audited)
	}
}
