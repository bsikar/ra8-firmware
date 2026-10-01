// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// fakePlane is the server half of the agent protocol, in memory. It exists so
// one test can drive the whole walk (claim, ack, heartbeat, logs, artifacts,
// receipt) against a party that actually enforces the contract, rather than a
// per-test handler that accepts whatever arrives and proves only that a
// request was made.
//
// It enforces what the real plane enforces and nothing it cannot know:
// every message is fenced to the live grant, the ack precedes any evidence,
// log sequences advance by one and replay a repeat idempotently, and the
// terminal receipt arrives once and ends the attempt.
type fakePlane struct {
	mu         sync.Mutex
	assignment protocol.Assignment
	handedOut  bool
	acked      bool
	heartbeats int
	logs       []protocol.LogChunk
	chunks     []protocol.ArtifactChunk
	manifests  []protocol.ArtifactManifest
	receipt    *protocol.TerminalReceipt
	cancel     bool
	violations []string
	// atReceipt is asked, at the moment the terminal receipt arrives,
	// whether the world outside the protocol is already in the state the
	// receipt claims. It answers "" when it is, and a violation otherwise.
	// Ordering assertions that need a real clock live here rather than
	// after the run, where a dead process proves nothing about when it died.
	atReceipt func() string
}

func newFakePlane(assignment protocol.Assignment) *fakePlane {
	return &fakePlane{assignment: assignment}
}

// cancelNextHeartbeat makes the plane ask the agent to give the runner back.
func (plane *fakePlane) cancelNextHeartbeat() {
	plane.mu.Lock()
	defer plane.mu.Unlock()
	plane.cancel = true
}

// acknowledge stands in for the ack a full walk performs. A test driving one
// phase on its own uses it so the plane still refuses evidence outside a
// grant it never acknowledged for every other test.
func (plane *fakePlane) acknowledge() {
	plane.mu.Lock()
	defer plane.mu.Unlock()
	plane.acked = true
}

func (plane *fakePlane) violate(format string, args ...any) {
	plane.violations = append(plane.violations, fmt.Sprintf(format, args...))
}

// fenced refuses anything not carrying the live grant, the way the real plane
// refuses a message from a superseded attempt.
func (plane *fakePlane) fenced(assignmentID, attemptID string, version, fence int64) bool {
	return assignmentID == plane.assignment.AssignmentID && attemptID == plane.assignment.AttemptID &&
		version == plane.assignment.AssignmentVersion && fence == plane.assignment.FencingToken
}

func (plane *fakePlane) accept(w http.ResponseWriter) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(protocol.AcceptResponse{SchemaVersion: protocol.Version,
		AssignmentVersion: plane.assignment.AssignmentVersion,
		FencingToken:      plane.assignment.FencingToken, Accepted: true})
}

func (plane *fakePlane) handler() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		plane.mu.Lock()
		defer plane.mu.Unlock()
		switch {
		case r.URL.Path == "/v1/agents/me/claim":
			plane.claim(w, r)
		case r.URL.Path == "/v1/assignments/"+plane.assignment.AssignmentID+"/ack":
			plane.ack(w, r)
		case r.URL.Path == "/v1/agents/me/heartbeat":
			plane.heartbeat(w, r)
		case strings.HasSuffix(r.URL.Path, "/logs"):
			plane.log(w, r)
		case strings.HasSuffix(r.URL.Path, "/artifacts/chunk"):
			plane.artifactChunk(w, r)
		case strings.HasSuffix(r.URL.Path, "/artifacts/manifest"):
			plane.artifactManifest(w, r)
		case strings.HasSuffix(r.URL.Path, "/result"):
			plane.result(w, r)
		default:
			plane.violate("unknown endpoint %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}
}

func (plane *fakePlane) claim(w http.ResponseWriter, r *http.Request) {
	var claim protocol.ClaimRequest
	if err := protocol.DecodeStrict(r.Body, &claim); err != nil || claim.Validate() != nil {
		plane.violate("invalid claim: %v", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}
	if plane.handedOut {
		// One grant per attempt: a second claim gets no work, never the
		// same attempt again.
		w.WriteHeader(http.StatusNoContent)
		return
	}
	plane.handedOut = true
	_ = json.NewEncoder(w).Encode(plane.assignment)
}

func (plane *fakePlane) ack(w http.ResponseWriter, r *http.Request) {
	var ack protocol.Ack
	if err := protocol.DecodeStrict(r.Body, &ack); err != nil || ack.Validate() != nil {
		plane.violate("invalid ack: %v", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}
	if !plane.fenced(ack.AssignmentID, ack.AttemptID, ack.AssignmentVersion, ack.FencingToken) {
		plane.violate("ack is fenced to another grant")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if ack.CatalogSHA256 != plane.assignment.CatalogSHA256 ||
		ack.SourceSnapshotSHA256 != plane.assignment.Source.SnapshotSHA256 {
		plane.violate("ack accepted a different catalog or source than the grant named")
		w.WriteHeader(http.StatusConflict)
		return
	}
	plane.acked = true
	plane.accept(w)
}

func (plane *fakePlane) heartbeat(w http.ResponseWriter, r *http.Request) {
	var beat protocol.Heartbeat
	if err := protocol.DecodeStrict(r.Body, &beat); err != nil || beat.Validate() != nil {
		plane.violate("invalid heartbeat: %v", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}
	if !plane.fenced(beat.AssignmentID, beat.AttemptID, beat.AssignmentVersion, beat.FencingToken) {
		plane.violate("heartbeat is fenced to another grant")
		w.WriteHeader(http.StatusConflict)
		return
	}
	plane.heartbeats++
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(protocol.HeartbeatResponse{SchemaVersion: protocol.Version,
		AssignmentVersion: plane.assignment.AssignmentVersion,
		FencingToken:      plane.assignment.FencingToken, Cancel: plane.cancel})
}

func (plane *fakePlane) log(w http.ResponseWriter, r *http.Request) {
	var chunk protocol.LogChunk
	if err := protocol.DecodeStrict(r.Body, &chunk); err != nil || chunk.Validate() != nil {
		plane.violate("invalid log chunk: %v", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}
	if !plane.fenced(chunk.AssignmentID, chunk.AttemptID, chunk.AssignmentVersion, chunk.FencingToken) {
		plane.violate("log chunk is fenced to another grant")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if !plane.acked {
		plane.violate("log chunk arrived before the ack")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if plane.receipt != nil {
		plane.violate("log chunk arrived after the terminal receipt")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if held := len(plane.logs); held > 0 {
		previous := plane.logs[held-1]
		switch {
		case chunk.Sequence == previous.Sequence:
			// An ambiguous chunk the agent retried. Byte-identical is
			// idempotent; anything else is a contradiction.
			if chunk.SHA256 != previous.SHA256 {
				plane.violate("sequence %d replayed with different bytes", chunk.Sequence)
				w.WriteHeader(http.StatusConflict)
				return
			}
			plane.accept(w)
			return
		case chunk.Sequence != previous.Sequence+1:
			plane.violate("log sequence jumped from %d to %d", previous.Sequence, chunk.Sequence)
			w.WriteHeader(http.StatusConflict)
			return
		}
	} else if chunk.Sequence != 1 {
		plane.violate("first log chunk is sequence %d, want 1", chunk.Sequence)
		w.WriteHeader(http.StatusConflict)
		return
	}
	plane.logs = append(plane.logs, chunk)
	plane.accept(w)
}

func (plane *fakePlane) artifactChunk(w http.ResponseWriter, r *http.Request) {
	var chunk protocol.ArtifactChunk
	if err := protocol.DecodeStrict(r.Body, &chunk); err != nil || chunk.Validate() != nil {
		plane.violate("invalid artifact chunk: %v", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}
	if !plane.fenced(chunk.AssignmentID, chunk.AttemptID, chunk.AssignmentVersion, chunk.FencingToken) {
		plane.violate("artifact chunk is fenced to another grant")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if !plane.acked {
		plane.violate("artifact chunk arrived before the ack")
		w.WriteHeader(http.StatusConflict)
		return
	}
	plane.chunks = append(plane.chunks, chunk)
	plane.accept(w)
}

func (plane *fakePlane) artifactManifest(w http.ResponseWriter, r *http.Request) {
	var manifest protocol.ArtifactManifest
	if err := protocol.DecodeStrict(r.Body, &manifest); err != nil || manifest.Validate() != nil {
		plane.violate("invalid artifact manifest: %v", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}
	if !plane.fenced(manifest.AssignmentID, manifest.AttemptID, manifest.AssignmentVersion, manifest.FencingToken) {
		plane.violate("artifact manifest is fenced to another grant")
		w.WriteHeader(http.StatusConflict)
		return
	}
	// A manifest closes bytes the plane already holds, so every chunk it
	// covers must have arrived first.
	covered := 0
	for _, chunk := range plane.chunks {
		if chunk.Path == manifest.Path && manifest.Covers(chunk) == nil {
			covered++
		}
	}
	if int64(covered) != manifest.FinalSequence {
		plane.violate("manifest for %q closes %d chunks, plane holds %d",
			manifest.Path, manifest.FinalSequence, covered)
		w.WriteHeader(http.StatusConflict)
		return
	}
	plane.manifests = append(plane.manifests, manifest)
	plane.accept(w)
}

func (plane *fakePlane) result(w http.ResponseWriter, r *http.Request) {
	var receipt protocol.TerminalReceipt
	if err := protocol.DecodeStrict(r.Body, &receipt); err != nil || receipt.Validate() != nil {
		plane.violate("invalid terminal receipt: %v", err)
		w.WriteHeader(http.StatusBadRequest)
		return
	}
	if !plane.fenced(receipt.AssignmentID, receipt.AttemptID, receipt.AssignmentVersion, receipt.FencingToken) {
		plane.violate("terminal receipt is fenced to another grant")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if !plane.acked {
		plane.violate("terminal receipt arrived before the ack")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if plane.receipt != nil {
		plane.violate("a second terminal receipt arrived for one attempt")
		w.WriteHeader(http.StatusConflict)
		return
	}
	if plane.atReceipt != nil {
		if note := plane.atReceipt(); note != "" {
			plane.violate("%s", note)
			w.WriteHeader(http.StatusConflict)
			return
		}
	}
	if held := len(plane.logs); int64(held) != receipt.FinalLogSequence {
		plane.violate("receipt claims final log sequence %d, plane holds %d chunks",
			receipt.FinalLogSequence, held)
		w.WriteHeader(http.StatusConflict)
		return
	}
	plane.receipt = &receipt
	plane.accept(w)
}

// artifacts reports what the plane holds about produced files, so a test can
// assert an attempt spent no upload at all.
func (plane *fakePlane) artifacts() (chunks, manifests int) {
	plane.mu.Lock()
	defer plane.mu.Unlock()
	return len(plane.chunks), len(plane.manifests)
}

// beats reports how many heartbeats arrived.
func (plane *fakePlane) beats() int {
	plane.mu.Lock()
	defer plane.mu.Unlock()
	return plane.heartbeats
}

// state is a snapshot a test can assert against without racing the handler.
func (plane *fakePlane) state() (acked bool, logs int, receipt *protocol.TerminalReceipt, violations []string) {
	plane.mu.Lock()
	defer plane.mu.Unlock()
	return plane.acked, len(plane.logs), plane.receipt, append([]string(nil), plane.violations...)
}

// planeFixture serves one fake plane and hands back an agent pointed at it.
func planeFixture(t *testing.T, plane *fakePlane) (*Agent, func()) {
	t.Helper()
	agent, server := testAgent(t, plane.handler())
	return agent, server.Close
}
