//go:build integration

package store

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/jackc/pgx/v5/pgxpool"
)

func dispatchFixture(t *testing.T) (*Store, *pgxpool.Pool, []byte, *catalog.Catalog, Run, protocol.HostFacts) {
	t.Helper()
	st, pool := integrationStore(t)
	ctx := context.Background()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	cert := []byte("dispatch-integration-" + mustID(t))
	fingerprint := sha256.Sum256(cert)
	principal := "dispatch-agent-" + mustID(t)
	agentID := mustID(t)
	repository := "bsikar/ra8ci-dispatch-test-" + mustID(t)
	_, err = pool.Exec(ctx, `INSERT INTO api_principals
		(cert_sha256,principal_id,kind,expires_at)
		VALUES ($1,$2,'agent',clock_timestamp()+interval '1 hour')`,
		hex.EncodeToString(fingerprint[:]), principal)
	if err != nil {
		t.Fatal(err)
	}
	_, err = pool.Exec(ctx, `INSERT INTO api_grants(principal_id,repository,role)
		VALUES ($1,$2,'agent_executor')`, principal, repository)
	if err != nil {
		t.Fatal(err)
	}
	_, err = pool.Exec(ctx, `INSERT INTO agents(id,principal_id,host_class,version,
		capabilities,capacity,state) VALUES ($1,$2,'linux-vm','test',
		'{"os":"linux"}'::jsonb,1,'healthy')`, agentID, principal)
	if err != nil {
		t.Fatal(err)
	}
	run, err := st.CreateRun(ctx, CreateRunInput{
		Trigger: "integration", ActorID: "integration-submitter",
		Repository: repository, Branch: "test",
		CommitSHA: strings.Repeat("a", 40), SnapshotSHA256: strings.Repeat("b", 64),
		CatalogSHA256: cat.Digest(), Tasks: []TaskInput{{Key: "format-check", Name: "format-check",
			Arguments: json.RawMessage(`{"argv":[]}`), Tier: "required",
			Scope: "safe-local-read-only", HostClass: "safe-local-read-only", DeadlineSeconds: 900}},
	})
	if err != nil {
		t.Fatal(err)
	}
	facts := protocol.HostFacts{Cores: 4, RAMBytes: 8 << 30, RAMFreeBytes: 4 << 30,
		Load1: 0.5, LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: time.Now().UTC()}
	return st, pool, cert, cat, run, facts
}

func TestIntegrationAgentDispatchEvidenceAndFence(t *testing.T) {
	st, pool, cert, cat, run, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	trustedCommit := strings.Repeat("a", 40)
	if _, err := st.ClaimAgentTask(ctx, []byte("unknown certificate"), facts, cat, trustedCommit); !errors.Is(err, ErrDenied) {
		t.Fatalf("unregistered agent was not denied: %v", err)
	}
	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, trustedCommit)
	if err != nil || grant == nil || grant.Validate() != nil || grant.Task.Name != "format-check" {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	if grant.Source.SnapshotSHA256 != run.SnapshotSHA256 || grant.CatalogSHA256 != cat.Digest() {
		t.Fatalf("grant changed source or catalog: %+v", grant)
	}
	replayed, err := st.ClaimAgentTask(ctx, cert, facts, cat, trustedCommit)
	if err != nil || replayed == nil || replayed.AssignmentID != grant.AssignmentID ||
		replayed.FencingToken != grant.FencingToken {
		t.Fatalf("lost claim response was not safely replayed: %+v, %v", replayed, err)
	}
	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
	stale := ack
	stale.FencingToken++
	if err := st.AcknowledgeAgentAssignment(ctx, cert, stale); !errors.Is(err, ErrConflict) {
		t.Fatalf("stale fence accepted: %v", err)
	}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); err != nil {
		t.Fatal(err)
	}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); err != nil {
		t.Fatalf("exact ACK replay failed: %v", err)
	}
	data := []byte("gate passed\n")
	sum := sha256.Sum256(data)
	chunk := protocol.LogChunk{SchemaVersion: protocol.Version,
		AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
		AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
		Sequence: 1, Stream: "stdout", StepName: "format-tree-check", DataBase64: base64.StdEncoding.EncodeToString(data),
		SHA256: hex.EncodeToString(sum[:])}
	if err := st.SaveAgentLog(ctx, cert, chunk); err != nil {
		t.Fatal(err)
	}
	if err := st.SaveAgentLog(ctx, cert, chunk); err != nil {
		t.Fatalf("exact log replay failed: %v", err)
	}
	changed := chunk
	changed.Stream = "stderr"
	if err := st.SaveAgentLog(ctx, cert, changed); !errors.Is(err, ErrConflict) {
		t.Fatalf("altered log replay accepted: %v", err)
	}
	gap := chunk
	gap.Sequence = 3
	if err := st.SaveAgentLog(ctx, cert, gap); !errors.Is(err, ErrConflict) {
		t.Fatalf("log gap accepted: %v", err)
	}
	heartbeat := protocol.Heartbeat{SchemaVersion: protocol.Version,
		AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
		AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
		Phase: "executing", HostFacts: facts}
	response, err := st.HeartbeatAgentAttempt(ctx, cert, heartbeat)
	if err != nil || response.Cancel || response.Yield {
		t.Fatalf("healthy heartbeat failed: %+v, %v", response, err)
	}
	started := time.Now().UTC().Add(-time.Second)
	ended := time.Now().UTC()
	zero := 0
	emptySum := sha256.Sum256(nil)
	receipt := protocol.TerminalReceipt{SchemaVersion: protocol.Version,
		AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
		AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
		Outcome: "succeeded", ChildExitCode: &zero, EvidenceComplete: true,
		StartedAt: started, EndedAt: ended, DurationNS: ended.Sub(started).Nanoseconds(),
		Steps: []protocol.StepSummary{{Name: "format-tree-check", StartedAt: started,
			EndedAt: ended, DurationNS: ended.Sub(started).Nanoseconds(), ExitCode: 0,
			StdoutSHA256: hex.EncodeToString(sum[:]), StderrSHA256: hex.EncodeToString(emptySum[:]),
			StdoutBytes: int64(len(data))}},
		FinalLogSequence: 1, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256,
		HostFactsAtStart:     facts, HostFactsAtEnd: facts}
	alteredReceipt := receipt
	alteredReceipt.Steps = append([]protocol.StepSummary(nil), receipt.Steps...)
	alteredReceipt.Steps[0].StderrSHA256 = strings.Repeat("e", 64)
	if err := st.CompleteAgentAttempt(ctx, cert, alteredReceipt, cat); !errors.Is(err, ErrConflict) {
		t.Fatalf("receipt with forged stderr digest accepted: %v", err)
	}
	if err := st.CompleteAgentAttempt(ctx, cert, receipt, cat); err != nil {
		t.Fatal(err)
	}
	finished, err := st.GetRun(ctx, run.ID)
	if err != nil || finished.State != "terminal" || finished.ExecutionResult != "succeeded" ||
		finished.EvidenceState != "complete" {
		t.Fatalf("run did not close with evidence: %+v, %v", finished, err)
	}
	var resourceCount int
	var hostOS string
	if err := pool.QueryRow(ctx, "SELECT COUNT(*)::int, MIN(host_os) FROM resource_samples WHERE attempt_id=$1", grant.AttemptID).Scan(&resourceCount, &hostOS); err != nil {
		t.Fatal(err)
	}
	if resourceCount < 3 || hostOS != "linux" {
		t.Fatalf("resource samples were not persisted: count=%d host_os=%q", resourceCount, hostOS)
	}
	if err := st.CompleteAgentAttempt(ctx, cert, receipt, cat); !errors.Is(err, ErrConflict) {
		t.Fatalf("terminal replay did not conflict: %v", err)
	}
	var assigned, terminal int
	if err := pool.QueryRow(ctx, `SELECT
		COUNT(*) FILTER (WHERE action='task.assigned'),
		COUNT(*) FILTER (WHERE action='task.attempt.finished')
		FROM audit WHERE correlation_run_id=$1`, run.ID).Scan(&assigned, &terminal); err != nil || assigned != 1 || terminal != 1 {
		t.Fatalf("missing assignment/terminal audit: %d/%d %v", assigned, terminal, err)
	}
}

func TestIntegrationExpiredAgentAssignmentIsFencedAndRunClosed(t *testing.T) {
	st, pool, cert, cat, run, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, strings.Repeat("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim for reaper failed: %+v %v", grant, err)
	}
	_, err = pool.Exec(ctx, `UPDATE task_attempts SET deadline_at=clock_timestamp()-interval '2 minutes'
		WHERE id=$1`, grant.AttemptID)
	if err != nil {
		t.Fatal(err)
	}
	count, err := st.ReapAgentAssignments(ctx, cat, 100)
	if err != nil || count < 1 {
		t.Fatalf("expired assignment not reaped: %d %v", count, err)
	}
	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); !errors.Is(err, ErrConflict) {
		t.Fatalf("late ACK after reaping was accepted: %v", err)
	}
	finished, err := st.GetRun(ctx, run.ID)
	if err != nil || finished.State != "terminal" ||
		finished.ExecutionResult != "incomplete_evidence" || finished.Tasks[0].State != "lost" {
		t.Fatalf("expired run not closed with lost evidence: %+v %v", finished, err)
	}
}
