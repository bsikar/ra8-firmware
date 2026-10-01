//go:build integration

package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/migrations"
	"github.com/jackc/pgx/v5/pgxpool"
)

func integrationStore(t *testing.T) (*Store, *pgxpool.Pool) {
	t.Helper()
	dsn := os.Getenv("RA8CI_TEST_PG_DSN")
	if dsn == "" {
		t.Fatal("RA8CI_TEST_PG_DSN is required for integration tests")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatal(err)
	}
	if config.ConnConfig.Host != "127.0.0.1" || config.ConnConfig.Database != "ra8ci_test" {
		t.Fatalf("integration database must be disposable loopback ra8ci_test, got host=%q database=%q", config.ConnConfig.Host, config.ConnConfig.Database)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	if err := migrations.Apply(ctx, pool); err != nil {
		pool.Close()
		t.Fatal(err)
	}
	if err := migrations.Apply(ctx, pool); err != nil {
		pool.Close()
		t.Fatalf("idempotent migration failed: %v", err)
	}
	roleTx, err := pool.Begin(ctx)
	if err != nil {
		pool.Close()
		t.Fatal(err)
	}
	defer func() { _ = roleTx.Rollback(ctx) }()
	if _, err = roleTx.Exec(ctx, "SELECT pg_advisory_xact_lock(72628802)"); err != nil {
		_ = roleTx.Rollback(ctx)
		pool.Close()
		t.Fatal(err)
	}
	_, err = roleTx.Exec(ctx, `DO $$ BEGIN
		IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='ra8ci_runtime_test') THEN
			CREATE ROLE ra8ci_runtime_test LOGIN PASSWORD 'ra8ci_runtime_test_only';
		END IF;
	END $$`)
	if err != nil {
		_ = roleTx.Rollback(ctx)
		pool.Close()
		t.Fatal(err)
	}
	_, err = roleTx.Exec(ctx, `GRANT CONNECT ON DATABASE ra8ci_test TO ra8ci_runtime_test;
		GRANT USAGE ON SCHEMA public TO ra8ci_runtime_test;
		GRANT SELECT,INSERT,UPDATE ON ALL TABLES IN SCHEMA public TO ra8ci_runtime_test;
		REVOKE INSERT,UPDATE ON schema_migrations,board_fixture_profiles FROM ra8ci_runtime_test;
		REVOKE INSERT,UPDATE,DELETE,TRUNCATE ON api_principals,api_grants,agents,
			board_fixture_profiles,schema_migrations FROM ra8ci_runtime_test;
		REVOKE DELETE,TRUNCATE ON runner_vms,runner_vm_operations FROM ra8ci_runtime_test;
		REVOKE UPDATE ON audit,board_events,run_events,local_runs,local_run_steps,hil_observations FROM ra8ci_runtime_test`)
	if err != nil {
		_ = roleTx.Rollback(ctx)
		pool.Close()
		t.Fatal(err)
	}
	if err := roleTx.Commit(ctx); err != nil {
		_ = roleTx.Rollback(ctx)
		pool.Close()
		t.Fatal(err)
	}
	runtimeConfig, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		pool.Close()
		t.Fatal(err)
	}
	runtimeConfig.ConnConfig.User = "ra8ci_runtime_test"
	runtimeConfig.ConnConfig.Password = "ra8ci_runtime_test_only"
	runtimePool, err := pgxpool.NewWithConfig(ctx, runtimeConfig)
	if err != nil {
		pool.Close()
		t.Fatal(err)
	}
	s := &Store{pool: runtimePool}
	if err := s.CheckSchema(ctx); err != nil {
		runtimePool.Close()
		pool.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close(); pool.Close() })
	return s, pool
}

func TestIntegrationRunAndAudit(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := testRun()
	in.IdempotencyKey = "run-" + mustID(t)
	in.RequestSHA256 = strings.Repeat("d", 64)
	run, err := s.CreateRun(ctx, in)
	if err != nil {
		t.Fatal(err)
	}
	if run.State != "queued" || len(run.Tasks) != 2 {
		t.Fatalf("unexpected admitted run: %+v", run)
	}
	duplicate, err := s.CreateRun(ctx, in)
	if err != nil || duplicate.ID != run.ID {
		t.Fatalf("idempotent replay changed identity: %+v, %v", duplicate, err)
	}
	in.RequestSHA256 = strings.Repeat("e", 64)
	if _, err := s.CreateRun(ctx, in); !errors.Is(err, ErrConflict) {
		t.Fatalf("conflicting idempotency replay: %v", err)
	}
	byKey := make(map[string]Task)
	for _, task := range run.Tasks {
		byKey[task.Key] = task
	}
	if _, err := s.StartAttempt(ctx, testStart(byKey["test"].ID)); !errors.Is(err, ErrConflict) {
		t.Fatalf("dependent task started before prerequisite: %v", err)
	}
	first, err := s.StartAttempt(ctx, testStart(byKey["format"].ID))
	if err != nil {
		t.Fatal(err)
	}
	started := time.Now().UTC().Add(-time.Millisecond)
	ended := time.Now().UTC()
	zero := 0
	if err := s.RecordStep(ctx, StepInput{
		AttemptID: first.ID, ActorID: "tester", Key: "format", Ordinal: 0,
		Phase: "execute", StartedAt: started, EndedAt: ended,
		DurationNS: ended.Sub(started).Nanoseconds(), State: "succeeded", ChildExitCode: &zero,
	}); err != nil {
		t.Fatal(err)
	}
	if err := s.FinishAttempt(ctx, FinishAttemptInput{
		AttemptID: first.ID, ActorID: "tester", Result: "succeeded",
		ChildExitCode: &zero, EvidenceComplete: true,
	}); err != nil {
		t.Fatal(err)
	}
	second, err := s.StartAttempt(ctx, testStart(byKey["test"].ID))
	if err != nil {
		t.Fatal(err)
	}
	one := 1
	if err := s.FinishAttempt(ctx, FinishAttemptInput{
		AttemptID: second.ID, ActorID: "tester", Result: "failed",
		ChildExitCode: &one, EvidenceComplete: true, Reason: "test fixture",
	}); err != nil {
		t.Fatal(err)
	}
	result, err := s.GetRun(ctx, run.ID)
	if err != nil {
		t.Fatal(err)
	}
	if result.State != "terminal" || result.ExecutionResult != "failed" || result.EvidenceState != "complete" {
		t.Fatalf("incorrect terminal run: %+v", result)
	}
	events, err := s.GetRunEvents(ctx, run.ID, 0, 100)
	if err != nil || len(events) < 5 {
		t.Fatalf("missing durable events (%d): %v", len(events), err)
	}
	page, err := s.RunEvents(ctx, run.ID, 0, 2)
	if err != nil || len(page.Events) != 2 || !page.HasMore || page.NextAfter != 2 {
		t.Fatalf("first bounded event page is invalid: %+v error %v", page, err)
	}
	next, err := s.RunEvents(ctx, run.ID, page.NextAfter, 2)
	if err != nil || len(next.Events) != 2 || next.Events[0].Sequence != 3 || next.NextAfter != 4 {
		t.Fatalf("second bounded event page is invalid: %+v error %v", next, err)
	}
	if _, err := s.RunEvents(ctx, run.ID, 0, MaxEventPageSize+1); !errors.Is(err, ErrInvalid) {
		t.Fatalf("oversized run event page accepted: %v", err)
	}
	var auditCount int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM audit WHERE correlation_run_id=$1", run.ID).Scan(&auditCount); err != nil || auditCount < 5 {
		t.Fatalf("missing durable audit (%d): %v", auditCount, err)
	}
	if _, err := pool.Exec(ctx, "UPDATE audit SET outcome='tampered' WHERE correlation_run_id=$1", run.ID); err == nil {
		t.Fatal("audit UPDATE was not rejected")
	}
	if err := s.Health(ctx); err != nil {
		t.Fatalf("writable readiness failed: %v", err)
	}
}

func TestIntegrationDependencySkipAndInbox(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := testRun()
	run, err := s.CreateRun(ctx, in)
	if err != nil {
		t.Fatal(err)
	}
	first, err := s.StartAttempt(ctx, testStart(run.Tasks[0].ID))
	if err != nil {
		t.Fatal(err)
	}
	one := 1
	if err := s.FinishAttempt(ctx, FinishAttemptInput{
		AttemptID: first.ID, ActorID: "tester", Result: "failed", ChildExitCode: &one,
		EvidenceComplete: false, Reason: "lost log chunk",
	}); err != nil {
		t.Fatal(err)
	}
	finished, err := s.GetRun(ctx, run.ID)
	if err != nil {
		t.Fatal(err)
	}
	if finished.State != "terminal" || finished.ExecutionResult != "incomplete_evidence" {
		t.Fatalf("unjustified terminal result: %+v", finished)
	}
	for _, task := range finished.Tasks {
		if task.Key == "test" && task.State != "skipped" {
			t.Fatalf("failed prerequisite did not skip dependent: %+v", task)
		}
	}
	messageID := mustID(t)
	firstPayload := json.RawMessage(`{"type":"JobAvailable","stats":{"total":1}}`)
	if err := s.SaveGitHubMessage(ctx, "linux", "session", messageID, firstPayload); err != nil {
		t.Fatal(err)
	}
	if err := s.SaveGitHubMessage(ctx, "linux", "session", messageID, json.RawMessage(`{"stats":{"total":1},"type":"JobAvailable"}`)); err != nil {
		t.Fatalf("equivalent replay failed: %v", err)
	}
	if err := s.SaveGitHubMessage(ctx, "linux", "session", messageID, json.RawMessage(`{"type":"different"}`)); !errors.Is(err, ErrConflict) {
		t.Fatalf("conflicting replay was not rejected: %v", err)
	}
	pending, err := s.ListPendingGitHubMessages(ctx, 100)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, msg := range pending {
		if msg.MessageID == messageID {
			found = true
		}
	}
	if !found {
		t.Fatal("pending message absent from replay page")
	}
	foreignID := mustID(t)
	if err := s.SaveGitHubMessage(ctx, "windows", "session", foreignID, json.RawMessage(`{"type":"foreign"}`)); err != nil {
		t.Fatal(err)
	}
	filtered, err := s.ListPendingGitHubMessagesForScaleSet(ctx, "linux", 100)
	if err != nil {
		t.Fatal(err)
	}
	for _, msg := range filtered {
		if msg.ScaleSetID != "linux" || msg.MessageID == foreignID {
			t.Fatalf("foreign scale-set message leaked into replay: %+v", msg)
		}
	}
	if err := s.MarkGitHubMessageProcessed(ctx, "linux", "session", messageID); err != nil {
		t.Fatal(err)
	}
	if err := s.MarkGitHubMessageProcessed(ctx, "linux", "session", messageID); err != nil {
		t.Fatalf("processed replay failed: %v", err)
	}
}

func mustID(t *testing.T) string {
	t.Helper()
	id, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	return id
}

func testStart(taskID string) StartAttemptInput {
	return StartAttemptInput{
		TaskID: taskID, ActorID: "tester", Engine: "local-command", Host: "integration",
		HostCores: 4, HostRAMBytes: 8 * 1024 * 1024 * 1024, HostLoad: 0.2,
		HostFacts: json.RawMessage(fmt.Sprintf(`{"test":%q}`, "integration")),
	}
}
