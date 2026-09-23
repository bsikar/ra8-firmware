//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"sync"
	"testing"
	"time"
)

func testLocalRunInput() LocalRunInput {
	start := time.Now().UTC().Add(-2 * time.Second).Truncate(time.Microsecond)
	end := start.Add(time.Second)
	return LocalRunInput{
		PrincipalID: "local-principal", LocalID: strings.Repeat("a", 32),
		PayloadSHA256: strings.Repeat("b", 64), SourceVerification: "verified",
		Repository: "bsikar/ra8-firmware", Branch: "feature/offline",
		CommitSHA: strings.Repeat("c", 40), SnapshotSHA256: strings.Repeat("d", 64),
		CatalogSHA256: strings.Repeat("e", 64), TaskName: "format", Tier: "required",
		Scope: "safe-local-write-working-tree", DeadlineSeconds: 30,
		StartedAt: start, FinishedAt: end, DurationNS: int64(time.Second),
		Result: "succeeded", ChildExitCode: 0,
		Steps: []LocalStepInput{{
			Key: "format", Ordinal: 0, StartedAt: start.Add(time.Millisecond),
			EndedAt: end.Add(-time.Millisecond), DurationNS: int64(998 * time.Millisecond),
			StdoutSHA256: strings.Repeat("1", 64), StderrSHA256: strings.Repeat("2", 64),
		}},
	}
}

func TestIntegrationLocalRunPermanentReceiptAndAudit(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := testLocalRunInput()
	in.LocalID = strings.ReplaceAll(mustID(t), "-", "")
	first, err := s.IngestLocalRun(ctx, in)
	if err != nil || !ValidID(first.LocalRunID) || first.LocalID != in.LocalID || first.PayloadSHA256 != in.PayloadSHA256 {
		t.Fatalf("first receipt: %+v %v", first, err)
	}
	replayed, err := s.IngestLocalRun(ctx, in)
	if err != nil || replayed != first {
		t.Fatalf("replay changed receipt: %+v %v", replayed, err)
	}
	lookedUp, err := s.LookupLocalRunReceipt(ctx, in.PrincipalID, in.LocalID, in.PayloadSHA256)
	if err != nil || lookedUp != first {
		t.Fatalf("durable receipt lookup: %+v %v", lookedUp, err)
	}
	mutated := in
	mutated.PayloadSHA256 = strings.Repeat("f", 64)
	if _, err := s.LookupLocalRunReceipt(ctx, in.PrincipalID, in.LocalID, mutated.PayloadSHA256); !errors.Is(err, ErrConflict) {
		t.Fatalf("changed receipt lookup was accepted: %v", err)
	}
	if _, err := s.IngestLocalRun(ctx, mutated); !errors.Is(err, ErrConflict) {
		t.Fatalf("changed retry was accepted: %v", err)
	}
	var runCount, stepCount, auditCount int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM local_runs WHERE id=$1", first.LocalRunID).Scan(&runCount); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM local_run_steps WHERE local_run_id=$1", first.LocalRunID).Scan(&stepCount); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM audit WHERE target_type='local_run' AND target_id=$1", first.LocalRunID).Scan(&auditCount); err != nil {
		t.Fatal(err)
	}
	if runCount != 1 || stepCount != 1 || auditCount != 3 {
		t.Fatalf("atomic history: runs=%d steps=%d audit=%d", runCount, stepCount, auditCount)
	}
	var ciRuns int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM runs WHERE actor_id=$1", in.PrincipalID).Scan(&ciRuns); err != nil || ciRuns != 0 {
		t.Fatalf("local evidence became dispatchable CI: %d %v", ciRuns, err)
	}
	if _, err := s.pool.Exec(ctx, "TRUNCATE local_runs"); err == nil {
		t.Fatal("runtime role can truncate local history")
	}
}

func TestIntegrationLocalRunConcurrentReplayAndUnverified(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := testLocalRunInput()
	in.LocalID = strings.ReplaceAll(mustID(t), "-", "")
	in.SourceVerification = "unverified"
	in.SnapshotSHA256 = ""
	var wg sync.WaitGroup
	results := make(chan LocalRunReceipt, 2)
	errorsSeen := make(chan error, 2)
	for i := 0; i < 2; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			receipt, err := s.IngestLocalRun(ctx, in)
			results <- receipt
			errorsSeen <- err
		}()
	}
	wg.Wait()
	close(results)
	close(errorsSeen)
	var id string
	for err := range errorsSeen {
		if err != nil {
			t.Fatalf("concurrent retry: %v", err)
		}
	}
	for receipt := range results {
		if id != "" && id != receipt.LocalRunID {
			t.Fatalf("concurrent retry made two receipts: %s %s", id, receipt.LocalRunID)
		}
		id = receipt.LocalRunID
	}
	var verification string
	if err := pool.QueryRow(ctx, "SELECT source_verification FROM local_runs WHERE id=$1", id).Scan(&verification); err != nil || verification != "unverified" {
		t.Fatalf("unverified source was upgraded: %q %v", verification, err)
	}
	in.SnapshotSHA256 = strings.Repeat("d", 64)
	in.LocalID = strings.ReplaceAll(mustID(t), "-", "")
	if _, err := s.IngestLocalRun(ctx, in); err == nil {
		t.Fatal("unverified local run carried a fabricated snapshot digest")
	}
}
