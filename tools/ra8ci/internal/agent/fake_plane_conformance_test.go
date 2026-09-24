// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// TestFakePlaneDrivesTheFullProtocol is the issue's fake-agent drive: one real
// agent, one party enforcing the contract, and the whole walk in a single
// test rather than a handler per step that accepts whatever arrives.
func TestFakePlaneDrivesTheFullProtocol(t *testing.T) {
	root, snapshot := fixtureCheckout(t, "printf 'agent-log\\n'\n")
	assignment := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	assignment.CatalogSHA256 = definitions.Digest()
	assignment.Source.Commit, assignment.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest
	plane := newFakePlane(assignment)
	agent, closeServer := planeFixture(t, plane)
	defer closeServer()
	agent.root = root

	assigned, err := agent.RunOnce(context.Background())
	if err != nil || !assigned {
		t.Fatalf("run = %v, %v", assigned, err)
	}
	acked, logs, receipt, violations := plane.state()
	if len(violations) != 0 {
		t.Fatalf("the agent broke the contract: %v", violations)
	}
	if !acked || logs != 1 || receipt == nil {
		t.Fatalf("incomplete walk: acked=%v logs=%d receipt=%v", acked, logs, receipt)
	}
	if receipt.Outcome != "succeeded" || !receipt.EvidenceComplete || receipt.FinalLogSequence != 1 {
		t.Fatalf("terminal receipt = %+v", receipt)
	}
	if receipt.CatalogSHA256 != assignment.CatalogSHA256 ||
		receipt.SourceSnapshotSHA256 != assignment.Source.SnapshotSHA256 {
		t.Fatalf("receipt names a different catalog or source than the grant: %+v", receipt)
	}

	// A second claim gets no work: one grant, one attempt.
	assigned, err = agent.RunOnce(context.Background())
	if err != nil || assigned {
		t.Fatalf("second claim = %v, %v, want no work", assigned, err)
	}
}

// TestFakePlaneRefusesEvidenceOutsideTheGrant proves the harness enforces
// rather than accepts. A plane that took anything would make the drive above
// prove nothing at all.
func TestFakePlaneRefusesEvidenceOutsideTheGrant(t *testing.T) {
	assignment := testAssignment()
	plane := newFakePlane(assignment)
	agent, closeServer := planeFixture(t, plane)
	defer closeServer()

	payload := []byte("unfenced\n")
	sum := sha256.Sum256(payload)
	stale := protocol.LogChunk{SchemaVersion: protocol.Version,
		AssignmentID: assignment.AssignmentID, AttemptID: assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion, FencingToken: assignment.FencingToken + 1,
		Sequence: 1, Stream: "stdout", StepName: "compile",
		DataBase64: base64.StdEncoding.EncodeToString(payload), SHA256: hex.EncodeToString(sum[:])}
	if status := post(t, agent, "/v1/attempts/"+assignment.AttemptID+"/logs", stale); status != http.StatusConflict {
		t.Fatalf("stale fence accepted with %d, want 409", status)
	}

	// Correctly fenced, but the attempt was never acknowledged.
	fresh := stale
	fresh.FencingToken = assignment.FencingToken
	if status := post(t, agent, "/v1/attempts/"+assignment.AttemptID+"/logs", fresh); status != http.StatusConflict {
		t.Fatalf("evidence before the ack accepted with %d, want 409", status)
	}

	_, logs, receipt, violations := plane.state()
	if logs != 0 || receipt != nil {
		t.Fatalf("plane kept refused evidence: logs=%d receipt=%v", logs, receipt)
	}
	if len(violations) != 2 {
		t.Fatalf("violations = %v, want the stale fence and the missing ack", violations)
	}
}

// post sends one already-built message straight at the plane, bypassing the
// agent's own fencing so the plane's enforcement is what is under test.
func post(t *testing.T, agent *Agent, endpoint string, body any) int {
	t.Helper()
	data, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	request, err := http.NewRequest(http.MethodPost, agent.base+endpoint, bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Content-Type", "application/json")
	response, err := agent.client.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = response.Body.Close() }()
	return response.StatusCode
}
