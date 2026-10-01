// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The run read commands want a plane, not a database: the client loads its
// material from disk and verifies whatever it is handed. A mutual-TLS stand-in
// is enough to drive them end to end, so what an operator sees for a status,
// a cancellation, a page of events and a page of logs is pinned here.

const answeredRun = "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a5b"

// servingRuns answers run reads and points the environment at itself.
func servingRuns(t *testing.T, handler http.HandlerFunc) {
	t.Helper()
	servingReport(t, mintReportMaterial(t), handler)
}

// atPath serves one JSON body at one path and 404s everything else, so a
// command asking the wrong path fails loudly rather than reading a body meant
// for another endpoint. Bodies are built from the real store types, which is
// what keeps a stand-in honest against a client that refuses unknown fields.
func atPath(t *testing.T, path string, body any) http.HandlerFunc {
	t.Helper()
	return func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path != path {
			writer.WriteHeader(http.StatusNotFound)
			return
		}
		writer.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(writer).Encode(body); err != nil {
			t.Errorf("the stand-in plane could not answer: %v", err)
		}
	}
}

func queuedRun(id string) store.Run {
	return store.Run{ID: id, Trigger: "manual", ActorID: "operator",
		Repository: "bsikar/ra8-firmware", Branch: "ra8ci/dev",
		CommitSHA: strings.Repeat("a", 40), State: "queued",
		CreatedAt: time.Now().UTC().Truncate(time.Second), Version: 1}
}

func TestRunStatusSpeaksTheRunThePlaneAnswered(t *testing.T) {
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun, queuedRun(answeredRun)))

	spoke, err := spoken(t, func() error {
		return showRun(context.Background(), []string{answeredRun})
	})
	if err != nil {
		t.Fatalf("a served run was refused: %v", err)
	}
	var read store.Run
	if err := json.Unmarshal([]byte(spoke), &read); err != nil {
		t.Fatalf("stdout was not a run: %v (%q)", err, spoke)
	}
	if read.ID != answeredRun || read.State != "queued" || read.Branch != "ra8ci/dev" {
		t.Errorf("the operator was told %+v", read)
	}
}

// Two ways an answer can be about something other than what was asked, both
// refused before anything reaches the operator: a different run, and a state
// this plane has no machine for.
func TestRunStatusRefusesAnAnswerItDidNotAskFor(t *testing.T) {
	other := "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a99"

	t.Run("about another run", func(t *testing.T) {
		servingRuns(t, atPath(t, "/v1/runs/"+answeredRun, queuedRun(other)))
		spoke, err := spoken(t, func() error {
			return showRun(context.Background(), []string{answeredRun})
		})
		if err == nil {
			t.Fatalf("an answer about another run was accepted: %q", spoke)
		}
		if spoke != "" {
			t.Errorf("a refused read still spoke %q", spoke)
		}
	})

	t.Run("in a state the plane has no machine for", func(t *testing.T) {
		run := queuedRun(answeredRun)
		run.State = "vibing"
		servingRuns(t, atPath(t, "/v1/runs/"+answeredRun, run))
		if _, err := spoken(t, func() error {
			return showRun(context.Background(), []string{answeredRun})
		}); err == nil {
			t.Fatal("an unknown run state was accepted")
		}
	})
}

// A cancellation answer that records no cancellation is honest only about a
// run the plane had already closed. Anything else read back as cancelled
// would tell an operator a running job is stopping when it is not.
func TestRunCancelRequiresTheAnswerToShowACancellation(t *testing.T) {
	t.Run("a stamped cancellation is spoken", func(t *testing.T) {
		run := queuedRun(answeredRun)
		asked := time.Now().UTC().Truncate(time.Second)
		run.CancelRequestedAt = &asked
		run.CancelRequestedBy = "operator"
		servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/cancel", run))

		spoke, err := spoken(t, func() error {
			return cancelRun(context.Background(), []string{answeredRun})
		})
		if err != nil {
			t.Fatalf("a stamped cancellation was refused: %v", err)
		}
		if !strings.Contains(spoke, "cancel_requested_at") {
			t.Errorf("the operator was not shown the cancellation: %q", spoke)
		}
	})

	t.Run("an unstamped answer about a live run is refused", func(t *testing.T) {
		servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/cancel", queuedRun(answeredRun)))
		spoke, err := spoken(t, func() error {
			return cancelRun(context.Background(), []string{answeredRun})
		})
		if err == nil {
			t.Fatalf("an unstamped cancellation was accepted: %q", spoke)
		}
	})
}

func TestRunEventsSpeaksEachEventOnItsOwnLine(t *testing.T) {
	happened := time.Now().UTC().Truncate(time.Second)
	page := store.RunEventPage{RunID: answeredRun, NextAfter: 2, Events: []store.RunEvent{
		{Sequence: 1, ID: answeredRun, Kind: "run_created",
			Data: json.RawMessage(`{"trigger":"manual"}`), HappenedAt: happened},
		{Sequence: 2, ID: answeredRun, Kind: "run_queued",
			Data: json.RawMessage(`{"queue":"default"}`), HappenedAt: happened},
	}}
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/events", page))

	spoke, err := spoken(t, func() error {
		return showRunEvents(context.Background(), []string{answeredRun})
	})
	if err != nil {
		t.Fatalf("a served event page was refused: %v", err)
	}
	lines := strings.Split(strings.TrimSpace(spoke), "\n")
	if len(lines) != 2 {
		t.Fatalf("the operator got %d lines: %q", len(lines), spoke)
	}
	for index, line := range lines {
		var event store.RunEvent
		if err := json.Unmarshal([]byte(line), &event); err != nil {
			t.Fatalf("line %d was not an event: %v (%q)", index+1, err, line)
		}
		if event.Sequence != int64(index+1) {
			t.Errorf("line %d carried sequence %d", index+1, event.Sequence)
		}
	}
}

// The page's own cursor is checked against the events in it: a page claiming
// more while carrying a short list would have the command ask again from a
// cursor the plane never reached.
func TestRunEventsRefusesAPageThatContradictsItsCursor(t *testing.T) {
	happened := time.Now().UTC().Truncate(time.Second)
	page := store.RunEventPage{RunID: answeredRun, NextAfter: 1, HasMore: true,
		Events: []store.RunEvent{{Sequence: 1, ID: answeredRun, Kind: "run_created",
			Data: json.RawMessage(`{"trigger":"manual"}`), HappenedAt: happened}}}
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/events", page))

	if _, err := spoken(t, func() error {
		return showRunEvents(context.Background(), []string{answeredRun})
	}); err == nil {
		t.Fatal("a page claiming more than it carried was accepted")
	}
}

func logChunk(sequence int64, stream, text string, offset int64) store.LogRecord {
	sum := sha256.Sum256([]byte(text))
	return store.LogRecord{Sequence: sequence, Stream: stream, StepName: "build",
		MonotonicOffsetNS: offset, SHA256: hex.EncodeToString(sum[:]),
		DataBase64: base64.StdEncoding.EncodeToString([]byte(text))}
}

// Log chunks are written as bytes, not as JSON, because the point of the
// command is to reproduce the transcript. stdout carries the stdout stream.
func TestRunLogsWritesTheTranscriptRatherThanItsEnvelope(t *testing.T) {
	page := store.LogPage{AttemptID: answeredRun, NextAfter: 2, Chunks: []store.LogRecord{
		logChunk(1, "stdout", "configuring\n", 1000),
		logChunk(2, "stdout", "building\n", 2000),
	}}
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/logs", page))

	spoke, err := spoken(t, func() error {
		return showRunLogs(context.Background(), []string{answeredRun, answeredRun})
	})
	if err != nil {
		t.Fatalf("a served log page was refused: %v", err)
	}
	if spoke != "configuring\nbuilding\n" {
		t.Errorf("the transcript read %q", spoke)
	}
}

// A chunk whose bytes do not match the digest beside them is refused, and
// nothing from that page reaches the operator. A transcript is evidence, so a
// chunk that cannot be shown to be what the attempt wrote is not shown at all.
func TestRunLogsRefusesAChunkThatDoesNotMatchItsDigest(t *testing.T) {
	chunk := logChunk(1, "stdout", "configuring\n", 1000)
	chunk.DataBase64 = base64.StdEncoding.EncodeToString([]byte("something else\n"))
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/logs",
		store.LogPage{AttemptID: answeredRun, NextAfter: 1, Chunks: []store.LogRecord{chunk}}))

	spoke, err := spoken(t, func() error {
		return showRunLogs(context.Background(), []string{answeredRun, answeredRun})
	})
	if err == nil {
		t.Fatalf("a mismatched digest was accepted: %q", spoke)
	}
	if spoke != "" {
		t.Errorf("a refused page still wrote %q to the transcript", spoke)
	}
}

// A chunk stamped before the one in front of it would put the transcript out
// of order once stdout and stderr are interleaved, so the page is refused.
func TestRunLogsRefusesAChunkStampedBeforeTheOneBeforeIt(t *testing.T) {
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/logs",
		store.LogPage{AttemptID: answeredRun, NextAfter: 2, Chunks: []store.LogRecord{
			logChunk(1, "stdout", "configuring\n", 9000),
			logChunk(2, "stdout", "building\n", 1000),
		}}))

	if _, err := spoken(t, func() error {
		return showRunLogs(context.Background(), []string{answeredRun, answeredRun})
	}); err == nil {
		t.Fatal("a chunk stamped out of order was accepted")
	}
}

// Every read command refuses an argument shape it cannot send before a
// request is spent.
func TestTheRunReadCommandsRefuseAnUnusableInvocation(t *testing.T) {
	for _, testCase := range []struct {
		name string
		run  func() error
	}{
		{"status with no run", func() error { return showRun(context.Background(), nil) }},
		{"status with two runs", func() error {
			return showRun(context.Background(), []string{answeredRun, answeredRun})
		}},
		{"cancel with a run that is not an identifier", func() error {
			return cancelRun(context.Background(), []string{"run-1"})
		}},
		{"events with no run", func() error { return showRunEvents(context.Background(), nil) }},
		{"events with an unknown flag", func() error {
			return showRunEvents(context.Background(), []string{"--page", "2", answeredRun})
		}},
		{"logs with a run but no attempt", func() error {
			return showRunLogs(context.Background(), []string{answeredRun})
		}},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			if err := testCase.run(); err == nil {
				t.Fatal("the invocation was accepted")
			}
		})
	}
}
