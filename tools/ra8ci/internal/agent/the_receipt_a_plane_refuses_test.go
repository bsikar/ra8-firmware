// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// The terminal receipt is the only message that says how an attempt ended. An
// attempt whose work succeeded but whose receipt was refused has to be reported
// as a failure: the plane is still holding the attempt open and nothing else
// will ever tell it otherwise. These pin that the refusal is handed back rather
// than folded into the success of the run it describes.

// ranToAReceipt stands a plane up that answers the claim, the ack and the logs
// honestly and hands the terminal receipt to answerResult. The counter reports
// how many times the receipt was actually offered, which is what separates a
// plane refusing the receipt from the agent never filing one.
func ranToAReceipt(t *testing.T, answerResult http.HandlerFunc) (*Agent, *atomic.Int64) {
	t.Helper()
	root, snapshot := fixtureCheckout(t, "printf 'agent-log\\n'\n")
	assignment := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	assignment.CatalogSHA256 = definitions.Digest()
	assignment.Source.Commit = snapshot.RootCommit
	assignment.Source.SnapshotSHA256 = snapshot.Digest
	offered := &atomic.Int64{}
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agents/me/claim":
			_ = json.NewEncoder(w).Encode(assignment)
		case "/v1/assignments/" + assignment.AssignmentID + "/ack":
			writeAccepted(w, assignment)
		case "/v1/attempts/" + assignment.AttemptID + "/logs":
			writeAccepted(w, assignment)
		case "/v1/attempts/" + assignment.AttemptID + "/artifacts":
			writeAccepted(w, assignment)
		case "/v1/attempts/" + assignment.AttemptID + "/result":
			offered.Add(1)
			answerResult(w, r)
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	})
	t.Cleanup(server.Close)
	agent.root = root
	return agent, offered
}

func TestAReceiptThePlaneRefusesIsNotASuccessfulAttempt(t *testing.T) {
	agent, offered := ranToAReceipt(t, func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "the attempt is not on file here", http.StatusBadRequest)
	})
	assigned, err := agent.RunOnce(context.Background())
	if !assigned {
		t.Fatal("an assignment that ran and was refused its receipt was reported as no work")
	}
	if err == nil {
		t.Fatal("a refused terminal receipt was reported as a completed attempt")
	}
	if offered.Load() != 1 {
		t.Fatalf("receipt offered %d times, want exactly 1: accept does not retry a decision", offered.Load())
	}
	if !strings.Contains(err.Error(), "400") {
		t.Fatalf("the refusal does not name the status the plane answered with: %v", err)
	}
}

// A receipt the plane answers 200 to, but with an acknowledgment belonging to
// some other version of the assignment, is the plane refusing this attempt's
// word. It is a protocol failure, not a transport one, and must not be retried
// into a transport error that hides the refusal.
func TestAReceiptAcknowledgedForAnotherAssignmentIsRefused(t *testing.T) {
	agent, offered := ranToAReceipt(t, func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
			AssignmentVersion: 99, FencingToken: 99, Accepted: true})
	})
	_, err := agent.RunOnce(context.Background())
	if err == nil {
		t.Fatal("a stale acknowledgment of the terminal receipt was taken as acceptance")
	}
	if !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("a stale acknowledgment was not reported as a protocol failure: %v", err)
	}
	if offered.Load() != 1 {
		t.Fatalf("receipt offered %d times, want exactly 1: a refusal is not retried", offered.Load())
	}
}

// The refusal above has to survive the run being fine. This is the same plane
// answering honestly, so the only difference is the receipt endpoint, and it
// must come back clean: otherwise the two tests above would pass for a reason
// that has nothing to do with the receipt.
func TestTheSameAttemptFiledAgainstAPlaneThatAcceptsItIsClean(t *testing.T) {
	var accepted protocol.Assignment
	agent, offered := ranToAReceipt(t, func(w http.ResponseWriter, r *http.Request) {
		var receipt protocol.TerminalReceipt
		if err := protocol.DecodeStrict(r.Body, &receipt); err != nil || receipt.Validate() != nil {
			t.Errorf("the receipt offered was not a valid one: %+v, %v", receipt, err)
		}
		if receipt.Outcome != "succeeded" || !receipt.EvidenceComplete {
			t.Errorf("outcome = %q evidence complete = %v, want a clean succeeded receipt",
				receipt.Outcome, receipt.EvidenceComplete)
		}
		writeAccepted(w, accepted)
	})
	accepted = testAssignment()
	assigned, err := agent.RunOnce(context.Background())
	if err != nil || !assigned {
		t.Fatalf("run = %v, %v, want a clean attempt", assigned, err)
	}
	if offered.Load() != 1 {
		t.Fatalf("receipt offered %d times, want exactly 1", offered.Load())
	}
}
