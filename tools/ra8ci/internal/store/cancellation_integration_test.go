//go:build integration

package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

func TestIntegrationQueuedRunCancellationIsDurableAndIdempotent(t *testing.T) {
	st, pool, _, _, run, _ := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	first, err := st.RequestRunCancellation(ctx, run.ID, "integration-human")
	if err != nil || first.State != "terminal" || first.ExecutionResult != "cancelled" ||
		first.CancelRequestedAt == nil || first.CancelRequestedBy != "integration-human" || len(first.Tasks) != 1 ||
		first.Tasks[0].State != "cancelled" {
		t.Fatalf("queued cancellation = %+v, %v", first, err)
	}
	second, err := st.RequestRunCancellation(ctx, run.ID, "integration-human")
	if err != nil || second.CancelRequestedAt == nil || !second.CancelRequestedAt.Equal(*first.CancelRequestedAt) {
		t.Fatalf("cancellation replay changed durable intent: %+v, %v", second, err)
	}
	var requests, taskCancels int
	if err := pool.QueryRow(ctx, `SELECT
		COUNT(*) FILTER (WHERE action='run.cancel.requested'),
		COUNT(*) FILTER (WHERE action='task.cancelled_before_assignment')
		FROM audit WHERE correlation_run_id=$1`, run.ID).Scan(&requests, &taskCancels); err != nil || requests != 1 || taskCancels != 1 {
		t.Fatalf("cancellation audit counts = %d/%d, %v", requests, taskCancels, err)
	}
}

func TestIntegrationCancellationBeforeAgentAckPreventsExecution(t *testing.T) {
	st, _, cert, cat, run, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, strings.Repeat("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	if _, err := st.RequestRunCancellation(ctx, run.ID, "integration-human"); err != nil {
		t.Fatal(err)
	}
	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); !errors.Is(err, ErrConflict) {
		t.Fatalf("cancelled assignment was acknowledged: %v", err)
	}
	finished, err := st.GetRun(ctx, run.ID)
	if err != nil || finished.State != "terminal" || finished.ExecutionResult != "cancelled" ||
		finished.Tasks[0].State != "cancelled" {
		t.Fatalf("pre-ACK cancellation did not close run: %+v, %v", finished, err)
	}
}

func TestIntegrationActiveRunCancellationUsesHeartbeatAndReceipt(t *testing.T) {
	st, _, cert, cat, run, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, strings.Repeat("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); err != nil {
		t.Fatal(err)
	}
	requested, err := st.RequestRunCancellation(ctx, run.ID, "integration-human")
	if err != nil || requested.State != "running" || requested.CancelRequestedAt == nil {
		t.Fatalf("active run cancellation = %+v, %v", requested, err)
	}
	heartbeat := protocol.Heartbeat{SchemaVersion: protocol.Version,
		AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
		AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
		Phase: "executing", HostFacts: facts}
	response, err := st.HeartbeatAgentAttempt(ctx, cert, heartbeat)
	if err != nil || !response.Cancel {
		t.Fatalf("cancellation intent was not delivered by heartbeat: %+v, %v", response, err)
	}
	started := time.Now().UTC().Add(-time.Millisecond)
	ended := time.Now().UTC()
	code := 130
	emptyHash := sha256.Sum256(nil)
	receipt := protocol.TerminalReceipt{SchemaVersion: protocol.Version,
		AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
		AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
		Outcome: "cancelled", ChildExitCode: &code, Cancelled: true, EvidenceComplete: true,
		StartedAt: started, EndedAt: ended, DurationNS: ended.Sub(started).Nanoseconds(),
		Steps: []protocol.StepSummary{{Name: "format-tree-check", StartedAt: started, EndedAt: ended,
			DurationNS: ended.Sub(started).Nanoseconds(), ExitCode: code, Cancelled: true,
			StdoutSHA256: hex.EncodeToString(emptyHash[:]), StderrSHA256: hex.EncodeToString(emptyHash[:])}},
		CatalogSHA256: grant.CatalogSHA256, SourceSnapshotSHA256: grant.Source.SnapshotSHA256,
		HostFactsAtStart: facts, HostFactsAtEnd: facts}
	if err := st.CompleteAgentAttempt(ctx, cert, receipt, cat); err != nil {
		encoded, _ := json.Marshal(receipt)
		t.Fatalf("cancelled terminal receipt rejected: %v (%s)", err, encoded)
	}
	finished, err := st.GetRun(ctx, run.ID)
	if err != nil || finished.State != "terminal" || finished.ExecutionResult != "cancelled" ||
		finished.EvidenceState != "complete" || finished.Tasks[0].State != "cancelled" {
		t.Fatalf("active cancellation did not terminalize with evidence: %+v, %v", finished, err)
	}
}
