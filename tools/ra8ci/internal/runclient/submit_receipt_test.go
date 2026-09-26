// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const receiptRunID = "00000000-0000-7000-8000-000000000001"

func submissionInput() SubmitRequest {
	return SubmitRequest{
		Trigger:       "cli",
		Source:        Source{Repository: "bsikar/ra8-firmware", CommitSHA: strings.Repeat("a", 40)},
		CatalogDigest: strings.Repeat("b", 64),
		Tasks:         []Task{{Key: "task-001", Name: "test-go", Args: []string{}}},
	}
}

// answering serves one submission receipt body, the way the server answers a
// fresh admission and an idempotent replay alike: HTTP 201 with id and state.
func answering(t *testing.T, id, state string) *httptest.Server {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusCreated)
		_ = json.NewEncoder(w).Encode(Receipt{ID: id, State: state})
	}))
	t.Cleanup(server.Close)
	return server
}

// A retry under the recorded idempotency key is answered from the run as it
// stands, so the states a replay can carry are every state of the machine.
// This is the case the old "state must be queued" check broke.
func TestAReplayedSubmissionIsAcceptedInEveryStateARunCanHold(t *testing.T) {
	for _, state := range []string{"queued", "running", "terminal"} {
		t.Run(state, func(t *testing.T) {
			receipt, err := testClient(answering(t, receiptRunID, state)).
				Submit(context.Background(), "retry-7", submissionInput())
			if err != nil {
				t.Fatalf("replay in state %q refused: %v", state, err)
			}
			if receipt.ID != receiptRunID || receipt.State != state {
				t.Fatalf("receipt = %+v, want id %q state %q", receipt, receiptRunID, state)
			}
		})
	}
}

// The harm of the old refusal was not the error, it was the lost identifier:
// the run exists, and the ID in the answer is the only way back to it.
func TestAStartedRunKeepsItsIdentifierThroughARetriedSubmission(t *testing.T) {
	receipt, err := testClient(answering(t, receiptRunID, "running")).
		Submit(context.Background(), "retry-7", submissionInput())
	if err != nil || receipt.ID == "" {
		t.Fatalf("Submit() = %+v, %v; a retry must hand back the run it replayed", receipt, err)
	}
	if !store.ValidID(receipt.ID) {
		t.Fatalf("receipt ID %q is not usable for Get, Cancel or Logs", receipt.ID)
	}
}

func TestAReceiptStateNoRunCanHoldIsRefused(t *testing.T) {
	for _, state := range []string{"", "Queued", "scheduled", "succeeded", "lost", "unknown"} {
		if err := checkSubmitReceipt(Receipt{ID: receiptRunID, State: state}); err == nil {
			t.Fatalf("state %q accepted; it is not a state of the run machine", state)
		}
	}
}

// Task and attempt states are near neighbours in the same tree and are NOT
// run states; the receipt is judged by the run machine alone.
func TestTheRunMachineIsWhatTheStateIsJudgedAgainst(t *testing.T) {
	for _, state := range []string{"queued", "running", "terminal"} {
		if !store.KnownRunState(state) {
			t.Fatalf("store.KnownRunState(%q) = false; the client leans on this", state)
		}
		if err := checkSubmitReceipt(Receipt{ID: receiptRunID, State: state}); err != nil {
			t.Fatalf("state %q refused: %v", state, err)
		}
	}
	if store.KnownRunState("scheduled") || store.KnownRunState("succeeded") {
		t.Fatal("a task outcome state is being reported as a run state")
	}
}

func TestAMalformedRunIDIsStillRefused(t *testing.T) {
	if err := checkSubmitReceipt(Receipt{ID: "bad", State: "queued"}); err == nil {
		t.Fatal("receipt with a malformed run ID accepted")
	}
	if err := checkSubmitReceipt(Receipt{ID: "", State: "queued"}); err == nil {
		t.Fatal("receipt with no run ID accepted")
	}
	if _, err := testClient(answering(t, "bad", "queued")).
		Submit(context.Background(), "retry-7", submissionInput()); err == nil {
		t.Fatal("Submit accepted a receipt with a malformed run ID")
	}
}

// The ID is checked first: a receipt that is wrong in both ways is reported as
// a bad identifier, the fact that decides whether anything is recoverable.
func TestTheIdentifierIsReportedBeforeTheState(t *testing.T) {
	err := checkSubmitReceipt(Receipt{ID: "bad", State: "nonsense"})
	if err == nil || !strings.Contains(err.Error(), "run ID") {
		t.Fatalf("error = %v, want the run ID named", err)
	}
}

func TestAnAbsurdStateIsNamedButBounded(t *testing.T) {
	err := checkSubmitReceipt(Receipt{ID: receiptRunID, State: strings.Repeat("q", 4096)})
	if err == nil {
		t.Fatal("4096-byte state accepted")
	}
	if len(err.Error()) > 200 {
		t.Fatalf("error is %d bytes; a server string must not size the message", len(err.Error()))
	}
	if !strings.Contains(err.Error(), "...") {
		t.Fatalf("error = %q, want the quoted state truncated", err.Error())
	}
}
