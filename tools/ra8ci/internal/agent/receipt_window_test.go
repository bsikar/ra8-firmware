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
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// TestTheFinishingWindowsLeaveTheReceiptItsOwn pins the relation between the
// two budgets the finishing phase spends, over the package's own constants.
// An edit raising logFlushWindow to or past requestLimit puts the receipt back
// behind the optional half of the evidence, and nothing at runtime would say
// so.
func TestTheFinishingWindowsLeaveTheReceiptItsOwn(t *testing.T) {
	if !windowsLeaveTheReceiptItsOwn(logFlushWindow, requestLimit) {
		t.Fatalf("flush window %v does not leave the receipt's %v to itself", logFlushWindow, requestLimit)
	}
	cases := []struct {
		name    string
		flush   time.Duration
		receipt time.Duration
		want    bool
	}{
		{"ordinary", 3 * time.Second, 10 * time.Second, true},
		{"equal windows share the budget", 10 * time.Second, 10 * time.Second, false},
		{"flush outlasts the receipt", 11 * time.Second, 10 * time.Second, false},
		{"unbounded flush", 0, 10 * time.Second, false},
		{"negative flush", -time.Second, 10 * time.Second, false},
		{"unbounded receipt", 3 * time.Second, 0, false},
		{"one nanosecond apart still holds", 10*time.Second - 1, 10 * time.Second, true},
	}
	for _, testCase := range cases {
		if got := windowsLeaveTheReceiptItsOwn(testCase.flush, testCase.receipt); got != testCase.want {
			t.Errorf("%s: windows(%v, %v) = %v, want %v", testCase.name, testCase.flush, testCase.receipt, got, testCase.want)
		}
	}
}

// TestFlushWindowDefaultsToTheReviewedBound keeps the test-only field from
// changing what a real agent does: only a positive value overrides.
func TestFlushWindowDefaultsToTheReviewedBound(t *testing.T) {
	cases := []struct {
		name string
		set  time.Duration
		want time.Duration
	}{
		{"unset", 0, logFlushWindow},
		{"negative", -time.Second, logFlushWindow},
		{"test bound", 20 * time.Millisecond, 20 * time.Millisecond},
		{"longer than the default", time.Minute, time.Minute},
	}
	for _, testCase := range cases {
		agent := &Agent{flush: testCase.set}
		if got := agent.flushWindow(); got != testCase.want {
			t.Errorf("%s: flushWindow() = %v, want %v", testCase.name, got, testCase.want)
		}
	}
}

// TestAStalledFlushReturnsAndLeavesTheChunkPending is the uploader half: a
// plane that never answers the logs endpoint costs exactly the window it was
// given, and the chunk stays held rather than being counted as delivered.
func TestAStalledFlushReturnsAndLeavesTheChunkPending(t *testing.T) {
	assignment := testAssignment()
	release := make(chan struct{})
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		stall(r, release)
	})
	defer server.Close()
	// Released before the server is closed (defers run last first), so a
	// handler still holding its connection cannot outlive the test.
	defer close(release)
	uploader := &logUploader{agent: agent, ctx: context.Background(), assignment: assignment}
	held := logChunkFor(t, assignment, 4, "step-one", "stdout", []byte("held\n"))
	uploader.sequence, uploader.pending, uploader.err = 3, &held, context.DeadlineExceeded

	flushCtx, stop := context.WithTimeout(context.Background(), 40*time.Millisecond)
	defer stop()
	started := time.Now()
	uploader.flushGrace(flushCtx)
	spent := time.Since(started)

	if spent > time.Second {
		t.Fatalf("flush spent %v on a plane that never answered", spent)
	}
	sequence, err := uploader.status()
	if sequence != 3 || err == nil {
		t.Fatalf("a stalled flush moved the record: sequence=%d err=%v", sequence, err)
	}
	if uploader.pending == nil {
		t.Fatal("the held chunk was dropped without ever being accepted")
	}
}

// TestAStalledLogFlushStillLetsTheReceiptOut is the gap itself, driven end to
// end. The plane refuses every log chunk and then stops answering the logs
// endpoint altogether, which is what a stalled log store looks like from here.
// The receipt has to reach the plane anyway, naming the lost logs, because it
// is the only message that says how the attempt ended.
func TestAStalledLogFlushStillLetsTheReceiptOut(t *testing.T) {
	root, snapshot := fixtureCheckout(t, "printf 'agent-log\\n'\n")
	assignment := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	assignment.CatalogSHA256 = definitions.Digest()
	assignment.Source.Commit, assignment.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest

	var mu sync.Mutex
	logCalls := 0
	release := make(chan struct{})
	var receipt protocol.TerminalReceipt
	var receiptAt time.Time
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agents/me/claim":
			_ = json.NewEncoder(w).Encode(assignment)
		case "/v1/assignments/" + assignment.AssignmentID + "/ack":
			writeAccepted(w, assignment)
		case "/v1/attempts/" + assignment.AttemptID + "/logs":
			mu.Lock()
			logCalls++
			offers := logCalls
			mu.Unlock()
			// The run's own bounded retries get a refusal each, which is
			// what leaves one chunk held. Everything after that hangs.
			if offers > evidenceAttempts {
				stall(r, release)
				return
			}
			w.WriteHeader(http.StatusServiceUnavailable)
		case "/v1/attempts/" + assignment.AttemptID + "/result":
			var arrived protocol.TerminalReceipt
			if err := protocol.DecodeStrict(r.Body, &arrived); err != nil || arrived.Validate() != nil {
				t.Errorf("invalid terminal receipt: %+v, %v", arrived, err)
			}
			mu.Lock()
			receipt, receiptAt = arrived, time.Now()
			mu.Unlock()
			writeAccepted(w, assignment)
		default:
			t.Errorf("unexpected endpoint %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer server.Close()
	defer close(release)
	agent.root = root
	agent.flush = 40 * time.Millisecond

	started := time.Now()
	assigned, runErr := agent.RunOnce(context.Background())
	if !assigned {
		t.Fatalf("no assignment taken: %v", runErr)
	}
	// The attempt itself failed on its evidence, and RunOnce says so.
	if runErr == nil {
		t.Fatal("lost log evidence was reported as a clean attempt")
	}

	mu.Lock()
	defer mu.Unlock()
	if receipt.AttemptID != assignment.AttemptID {
		t.Fatalf("the plane never learned how the attempt ended: %+v", receipt)
	}
	if receipt.EvidenceComplete {
		t.Fatalf("receipt claims complete evidence after losing a log chunk: %+v", receipt)
	}
	if receipt.FinalLogSequence != 0 {
		t.Fatalf("receipt counts chunks the plane never accepted: %+v", receipt)
	}
	// The receipt went out on its own window: the stalled flush was bounded
	// by agent.flush, so the whole finishing phase cannot have spent
	// anything like requestLimit before the receipt arrived.
	if waited := receiptAt.Sub(started); waited >= requestLimit {
		t.Fatalf("receipt arrived after %v, at or past the window it should have had to itself", waited)
	}
}

// TestTheReceiptWindowIsNotTheFlushsLeftovers is the same relation stated on
// the other side: even a flush that spends its whole window leaves the receipt
// a window that has not started. A receipt posted on the flush's leftovers
// would fail against a plane that answers in ordinary time, and this fails if
// the two are ever wired back onto one context.
func TestTheReceiptWindowIsNotTheFlushsLeftovers(t *testing.T) {
	root, snapshot := fixtureCheckout(t, "printf 'agent-log\\n'\n")
	assignment := testAssignment()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	assignment.CatalogSHA256 = definitions.Digest()
	assignment.Source.Commit, assignment.Source.SnapshotSHA256 = snapshot.RootCommit, snapshot.Digest

	var mu sync.Mutex
	logCalls, resultCalls := 0, 0
	release := make(chan struct{})
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/agents/me/claim":
			_ = json.NewEncoder(w).Encode(assignment)
		case "/v1/assignments/" + assignment.AssignmentID + "/ack":
			writeAccepted(w, assignment)
		case "/v1/attempts/" + assignment.AttemptID + "/logs":
			mu.Lock()
			logCalls++
			offers := logCalls
			mu.Unlock()
			if offers > evidenceAttempts {
				stall(r, release)
				return
			}
			w.WriteHeader(http.StatusServiceUnavailable)
		case "/v1/attempts/" + assignment.AttemptID + "/result":
			// An ordinary plane, answering after a pause a shared window
			// spent on the flush would no longer have left.
			time.Sleep(60 * time.Millisecond)
			mu.Lock()
			resultCalls++
			mu.Unlock()
			writeAccepted(w, assignment)
		default:
			t.Errorf("unexpected endpoint %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer server.Close()
	defer close(release)
	agent.root = root
	agent.flush = 20 * time.Millisecond

	if _, err := agent.RunOnce(context.Background()); err == nil {
		t.Fatal("lost log evidence was reported as a clean attempt")
	}
	mu.Lock()
	defer mu.Unlock()
	if resultCalls != 1 {
		t.Fatalf("receipt posts = %d, want exactly one that the plane answered", resultCalls)
	}
}

// stall holds a request the way a plane whose log store has stopped draining
// does: the connection is accepted and nothing ever comes back. It ends when
// the caller gives up, when the test releases it, or at a bound that keeps a
// forgotten handler from outliving the run.
func stall(r *http.Request, release <-chan struct{}) {
	select {
	case <-r.Context().Done():
	case <-release:
	case <-time.After(10 * time.Second):
	}
}

// logChunkFor builds a chunk the way the uploader does, so a test can hold one
// back without driving a whole run to produce it.
func logChunkFor(t *testing.T, assignment protocol.Assignment, sequence int64, step, stream string, data []byte) protocol.LogChunk {
	t.Helper()
	chunk := protocol.LogChunk{SchemaVersion: protocol.Version,
		AssignmentID: assignment.AssignmentID, AttemptID: assignment.AttemptID,
		AssignmentVersion: assignment.AssignmentVersion, FencingToken: assignment.FencingToken,
		Sequence: sequence, Stream: stream, StepName: step,
		DataBase64: base64.StdEncoding.EncodeToString(data), SHA256: digestOfLog(data)}
	if err := chunk.Validate(); err != nil {
		t.Fatalf("test chunk is not a valid chunk: %v", err)
	}
	return chunk
}

func digestOfLog(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}
