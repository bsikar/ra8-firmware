// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// acceptFor writes the acknowledgment a live grant would earn.
func acceptFor(w http.ResponseWriter, assignment protocol.Assignment) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
		AssignmentVersion: assignment.AssignmentVersion, FencingToken: assignment.FencingToken,
		Accepted: true})
}

func TestRetryableEvidenceSeparatesSilenceFromRefusal(t *testing.T) {
	for _, silent := range []int{0, http.StatusInternalServerError, http.StatusBadGateway,
		http.StatusServiceUnavailable, http.StatusGatewayTimeout} {
		if !retryableEvidence(silent) {
			t.Fatalf("status %d treated as a decision", silent)
		}
	}
	for _, decided := range []int{http.StatusOK, http.StatusBadRequest, http.StatusUnauthorized,
		http.StatusForbidden, http.StatusNotFound, http.StatusConflict,
		http.StatusRequestEntityTooLarge, http.StatusTooManyRequests} {
		if retryableEvidence(decided) {
			t.Fatalf("status %d treated as silence", decided)
		}
	}
}

// A control plane that is briefly unreachable must not cost the attempt its
// logs. Before this, the first failed chunk poisoned the uploader for good and
// the write error travelled back into the step, failing a task that was passing.
func TestLogChunkSurvivesATransientPlane(t *testing.T) {
	assignment := testAssignment()
	var calls atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		if calls.Add(1) < int64(evidenceAttempts) {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		acceptFor(w, assignment)
	})
	defer server.Close()
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
	written, err := uploader.write("format-check", "stdout", []byte("compiling\n"))
	if err != nil || written != len("compiling\n") {
		t.Fatalf("transient plane lost the chunk: wrote %d, %v", written, err)
	}
	if got := calls.Load(); got != int64(evidenceAttempts) {
		t.Fatalf("offers = %d, want %d", got, evidenceAttempts)
	}
	sequence, logErr := uploader.status()
	if sequence != 1 || logErr != nil {
		t.Fatalf("sequence = %d, err = %v", sequence, logErr)
	}
	if uploader.pending != nil {
		t.Fatal("a chunk the plane accepted is still pending")
	}
}

// A refusal is a decision. Asking again cannot change it and would only spend
// the running task's budget, so it is surfaced on the first answer.
func TestLogChunkRefusalIsNotRepeated(t *testing.T) {
	for _, status := range []int{http.StatusBadRequest, http.StatusConflict, http.StatusForbidden} {
		assignment := testAssignment()
		var calls atomic.Int64
		agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
			calls.Add(1)
			w.WriteHeader(status)
		})
		uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
		written, err := uploader.write("format-check", "stderr", []byte("vet failed\n"))
		server.Close()
		if err == nil || written != 0 {
			t.Fatalf("status %d accepted: wrote %d, %v", status, written, err)
		}
		if got := calls.Load(); got != 1 {
			t.Fatalf("status %d was offered %d times, want 1", status, got)
		}
		if uploader.pending == nil || uploader.pending.Sequence != 1 {
			t.Fatalf("status %d left no pending chunk for the grace flush", status)
		}
		if sequence, logErr := uploader.status(); sequence != 0 || logErr == nil {
			t.Fatalf("status %d advanced the sequence: %d, %v", status, sequence, logErr)
		}
	}
}

// Fencing is the plane refusing this attempt's word. It is answered, so it is a
// decision even though the transport succeeded, and it is never repeated.
func TestLogChunkFencingIsNotRepeated(t *testing.T) {
	assignment := testAssignment()
	var calls atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
			AssignmentVersion: assignment.AssignmentVersion,
			FencingToken:      assignment.FencingToken, Accepted: false})
	})
	defer server.Close()
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
	if _, err := uploader.write("format-check", "stdout", []byte("x")); !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("negative acknowledgment = %v", err)
	}
	if got := calls.Load(); got != 1 {
		t.Fatalf("fencing was offered %d times, want 1", got)
	}
}

// Retry is bounded. A plane that stays down must not hold the step open.
func TestLogChunkGivesUpAfterABoundedNumberOfOffers(t *testing.T) {
	assignment := testAssignment()
	var calls atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.WriteHeader(http.StatusServiceUnavailable)
	})
	defer server.Close()
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
	if _, err := uploader.write("format-check", "stdout", []byte("y")); err == nil {
		t.Fatal("a plane that never answered was reported as accepting")
	}
	if got := calls.Load(); got != int64(evidenceAttempts) {
		t.Fatalf("offers = %d, want %d", got, evidenceAttempts)
	}
	// The uploader stays poisoned: the sticky error is what the terminal
	// receipt reports, and a later chunk would break the sequence anyway.
	before := calls.Load()
	if _, err := uploader.write("format-check", "stdout", []byte("z")); err == nil {
		t.Fatal("a poisoned uploader accepted a later chunk")
	}
	if calls.Load() != before {
		t.Fatal("a poisoned uploader spoke to the plane again")
	}
}

// An expired budget stops the retry immediately rather than sleeping out the
// backoff the task no longer has time for.
func TestEvidenceRetryStopsWithTheBudget(t *testing.T) {
	assignment := testAssignment()
	var calls atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.WriteHeader(http.StatusServiceUnavailable)
	})
	defer server.Close()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	started := time.Now()
	err := agent.acceptEvidence(ctx, assignment, "/v1/attempts/"+assignment.AttemptID+"/logs",
		protocol.Ack{SchemaVersion: protocol.Version})
	if err == nil {
		t.Fatal("a cancelled budget reported success")
	}
	if elapsed := time.Since(started); elapsed > evidenceBackoff {
		t.Fatalf("cancelled retry slept %v", elapsed)
	}
}

// The first offer of a chunk the plane accepts costs exactly one request, so
// the common path did not become chattier.
func TestAcceptedLogChunkIsOfferedOnce(t *testing.T) {
	assignment := testAssignment()
	var calls atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		acceptFor(w, assignment)
	})
	defer server.Close()
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
	if _, err := uploader.write("format-check", "stdout", []byte("ok\n")); err != nil {
		t.Fatalf("write = %v", err)
	}
	if got := calls.Load(); got != 1 {
		t.Fatalf("a healthy plane was asked %d times, want 1", got)
	}
}
