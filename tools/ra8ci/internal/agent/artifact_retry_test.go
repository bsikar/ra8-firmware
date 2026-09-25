// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// retryingArtifactPlane answers both artifact endpoints, failing the first
// failures offers of each with status, and counts every offer it saw.
type retryingArtifactPlane struct {
	status    int
	failures  int64
	chunkHits atomic.Int64
	closeHits atomic.Int64
	remaining atomic.Int64
}

func (plane *retryingArtifactPlane) handler(assignment protocol.Assignment) http.HandlerFunc {
	plane.remaining.Store(plane.failures)
	return func(w http.ResponseWriter, r *http.Request) {
		switch {
		case strings.HasSuffix(r.URL.Path, "/artifacts/chunk"):
			plane.chunkHits.Add(1)
		case strings.HasSuffix(r.URL.Path, "/artifacts/manifest"):
			plane.closeHits.Add(1)
		default:
			w.WriteHeader(http.StatusNotFound)
			return
		}
		if plane.remaining.Add(-1) >= 0 {
			w.WriteHeader(plane.status)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
			AssignmentVersion: assignment.AssignmentVersion,
			FencingToken:      assignment.FencingToken, Accepted: true})
	}
}

func retryTestChunk(assignment protocol.Assignment) protocol.ArtifactChunk {
	data := []byte("one line of evidence\n")
	digest := sha256.Sum256(data)
	return protocol.ArtifactChunk{SchemaVersion: protocol.Version,
		AssignmentID: assignment.AssignmentID, AttemptID: assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion, FencingToken: assignment.FencingToken,
		StepName: "format-check", Path: "build/report.txt", Sequence: 1, Offset: 0,
		DataBase64: base64.StdEncoding.EncodeToString(data),
		SHA256:     hex.EncodeToString(digest[:])}
}

// The store keeps ArtifactDuplicate, and the manifest handler answers a
// duplicate close with the same 200 as the first, both of them stated as
// making an agent retry safe. Until this slice no agent retry existed: one
// unanswered chunk ended the whole attempt's artifact collection.
func TestArtifactChunkSurvivesATransientPlane(t *testing.T) {
	assignment := testAssignment()
	plane := &retryingArtifactPlane{status: http.StatusServiceUnavailable,
		failures: int64(evidenceAttempts) - 1}
	agent, server := testAgent(t, plane.handler(assignment))
	defer server.Close()
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatal(err)
	}
	if err := uploader.send(context.Background(), retryTestChunk(assignment)); err != nil {
		t.Fatalf("transient plane lost the chunk: %v", err)
	}
	if got := plane.chunkHits.Load(); got != int64(evidenceAttempts) {
		t.Fatalf("offers = %d, want %d", got, evidenceAttempts)
	}
}

// A refusal is a decision about this artifact, so it is surfaced on the first
// answer rather than spending the bounded artifact window on repeats.
func TestArtifactChunkRefusalIsNotRepeated(t *testing.T) {
	for _, status := range []int{http.StatusConflict, http.StatusBadRequest, http.StatusForbidden} {
		assignment := testAssignment()
		plane := &retryingArtifactPlane{status: status, failures: 1 << 30}
		agent, server := testAgent(t, plane.handler(assignment))
		err := uploaderSend(t, agent, assignment, retryTestChunk(assignment))
		server.Close()
		if err == nil {
			t.Fatalf("status %d accepted", status)
		}
		if got := plane.chunkHits.Load(); got != 1 {
			t.Fatalf("status %d was offered %d times, want 1", status, got)
		}
	}
}

func uploaderSend(t *testing.T, agent *Agent, assignment protocol.Assignment, chunk protocol.ArtifactChunk) error {
	t.Helper()
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		t.Fatal(err)
	}
	return uploader.send(context.Background(), chunk)
}

// A healthy plane is asked exactly once per chunk, so the common path did not
// become chattier.
func TestAcceptedArtifactChunkIsOfferedOnce(t *testing.T) {
	assignment := testAssignment()
	plane := &retryingArtifactPlane{status: http.StatusServiceUnavailable}
	agent, server := testAgent(t, plane.handler(assignment))
	defer server.Close()
	if err := uploaderSend(t, agent, assignment, retryTestChunk(assignment)); err != nil {
		t.Fatalf("send = %v", err)
	}
	if got := plane.chunkHits.Load(); got != 1 {
		t.Fatalf("a healthy plane was asked %d times, want 1", got)
	}
}

// Retry is bounded here too: a plane that stays down must not hold the
// artifact window open past its budget.
func TestArtifactChunkGivesUpAfterABoundedNumberOfOffers(t *testing.T) {
	assignment := testAssignment()
	plane := &retryingArtifactPlane{status: http.StatusBadGateway, failures: 1 << 30}
	agent, server := testAgent(t, plane.handler(assignment))
	defer server.Close()
	if err := uploaderSend(t, agent, assignment, retryTestChunk(assignment)); err == nil {
		t.Fatal("a plane that never answered was reported as accepting")
	}
	if got := plane.chunkHits.Load(); got != int64(evidenceAttempts) {
		t.Fatalf("offers = %d, want %d", got, evidenceAttempts)
	}
}
