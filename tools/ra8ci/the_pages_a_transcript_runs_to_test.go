// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// the_run_reads_an_operator_gets_test.go pins one page. A real attempt is
// several, and the paging is where a transcript goes wrong quietly: a command
// that forgets to carry the cursor forward repeats the first page for as long
// as the plane says there is more.

// heardOnBothStreams runs a command with both standard streams captured, so a
// chunk's stream can be checked where it actually lands rather than inferred
// from the envelope. spoken() next door captures stdout alone.
func heardOnBothStreams(t *testing.T, run func() error) (string, string, error) {
	t.Helper()
	outReader, outWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	errReader, errWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	savedOut, savedErr := os.Stdout, os.Stderr
	os.Stdout, os.Stderr = outWriter, errWriter
	runErr := run()
	os.Stdout, os.Stderr = savedOut, savedErr
	if err := outWriter.Close(); err != nil {
		t.Fatal(err)
	}
	if err := errWriter.Close(); err != nil {
		t.Fatal(err)
	}
	spoke, err := io.ReadAll(outReader)
	if err != nil {
		t.Fatal(err)
	}
	muttered, err := io.ReadAll(errReader)
	if err != nil {
		t.Fatal(err)
	}
	if err := outReader.Close(); err != nil {
		t.Fatal(err)
	}
	if err := errReader.Close(); err != nil {
		t.Fatal(err)
	}
	return string(spoke), string(muttered), runErr
}

// pagedRunPlane answers one path with a queue of pages and records the cursor
// each request asked from, which is the whole point: the second request has to
// start where the first page ended.
type pagedRunPlane struct {
	mu          sync.Mutex
	pages       []any
	asked       []string
	servingPath string
}

func (p *pagedRunPlane) serve(t *testing.T) http.HandlerFunc {
	t.Helper()
	return func(writer http.ResponseWriter, request *http.Request) {
		p.mu.Lock()
		defer p.mu.Unlock()
		if request.URL.Path != p.servingPath {
			writer.WriteHeader(http.StatusNotFound)
			return
		}
		p.asked = append(p.asked, request.URL.Query().Get("after"))
		if len(p.pages) == 0 {
			t.Errorf("the command asked for a page after the plane said there were none left")
			writer.WriteHeader(http.StatusInternalServerError)
			return
		}
		page := p.pages[0]
		p.pages = p.pages[1:]
		writer.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(writer).Encode(page); err != nil {
			t.Errorf("the stand-in plane could not answer: %v", err)
		}
	}
}

// A transcript longer than one page is read to its end, each page asked for
// from the cursor the one before it ended at. Getting this wrong is not a
// visible failure: the operator gets the first page over and over.
func TestRunLogsReadsATranscriptPastItsFirstPage(t *testing.T) {
	plane := &pagedRunPlane{servingPath: "/v1/runs/" + answeredRun + "/logs", pages: []any{
		store.LogPage{AttemptID: answeredRun, NextAfter: 1, HasMore: true,
			Chunks: []store.LogRecord{logChunk(1, "stdout", "configuring\n", 1000)}},
		store.LogPage{AttemptID: answeredRun, NextAfter: 2,
			Chunks: []store.LogRecord{logChunk(2, "stdout", "building\n", 2000)}},
	}}
	servingRuns(t, plane.serve(t))

	spoke, _, err := heardOnBothStreams(t, func() error {
		return showRunLogs(context.Background(), []string{"--limit", "1", answeredRun, answeredRun})
	})
	if err != nil {
		t.Fatalf("a two-page transcript was refused: %v", err)
	}
	if spoke != "configuring\nbuilding\n" {
		t.Fatalf("the operator got %q, want both pages in order", spoke)
	}
	plane.mu.Lock()
	asked := append([]string(nil), plane.asked...)
	plane.mu.Unlock()
	if len(asked) != 2 || asked[0] != "0" || asked[1] != "1" {
		t.Fatalf("the plane was asked from cursors %v, want the second page asked from 1", asked)
	}
}

// stderr belongs on stderr. A command that folded it into stdout would hand a
// caller capturing the transcript a stream the attempt never wrote there, and
// the two are interleaved by the attempt's own clock, not by the reader's.
func TestRunLogsPutsEachChunkOnTheStreamItWasWrittenTo(t *testing.T) {
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/logs",
		store.LogPage{AttemptID: answeredRun, NextAfter: 3, Chunks: []store.LogRecord{
			logChunk(1, "stdout", "configuring\n", 1000),
			logChunk(2, "stderr", "warning: stub crypto\n", 2000),
			logChunk(3, "stdout", "done\n", 3000),
		}}))

	spoke, muttered, err := heardOnBothStreams(t, func() error {
		return showRunLogs(context.Background(), []string{answeredRun, answeredRun})
	})
	if err != nil {
		t.Fatalf("a mixed transcript was refused: %v", err)
	}
	if spoke != "configuring\ndone\n" {
		t.Fatalf("stdout carried %q, want only what the attempt wrote there", spoke)
	}
	if muttered != "warning: stub crypto\n" {
		t.Fatalf("stderr carried %q, want the stderr chunk and nothing else", muttered)
	}
}

// The same paging, for events. Each page is one line per event, and the
// second page is asked for from where the first ended.
func TestRunEventsReadsAnEventLogPastItsFirstPage(t *testing.T) {
	happened := time.Now().UTC().Truncate(time.Second)
	event := func(sequence int64, kind string) store.RunEvent {
		return store.RunEvent{Sequence: sequence, ID: answeredRun, Kind: kind,
			Data: json.RawMessage(`{"trigger":"manual"}`), HappenedAt: happened}
	}
	plane := &pagedRunPlane{servingPath: "/v1/runs/" + answeredRun + "/events", pages: []any{
		store.RunEventPage{RunID: answeredRun, NextAfter: 1, HasMore: true,
			Events: []store.RunEvent{event(1, "run_created")}},
		store.RunEventPage{RunID: answeredRun, NextAfter: 2,
			Events: []store.RunEvent{event(2, "run_queued")}},
	}}
	servingRuns(t, plane.serve(t))

	spoke, _, err := heardOnBothStreams(t, func() error {
		return showRunEvents(context.Background(), []string{"--limit", "1", answeredRun})
	})
	if err != nil {
		t.Fatalf("a two-page event log was refused: %v", err)
	}
	lines := strings.Split(strings.TrimSpace(spoke), "\n")
	if len(lines) != 2 {
		t.Fatalf("the operator got %d lines: %q", len(lines), spoke)
	}
	for index, line := range lines {
		var got store.RunEvent
		if err := json.Unmarshal([]byte(line), &got); err != nil {
			t.Fatalf("line %d was not an event: %v (%q)", index+1, err, line)
		}
		if got.Sequence != int64(index+1) {
			t.Errorf("line %d carried sequence %d", index+1, got.Sequence)
		}
	}
	plane.mu.Lock()
	asked := append([]string(nil), plane.asked...)
	plane.mu.Unlock()
	if len(asked) != 2 || asked[0] != "0" || asked[1] != "1" {
		t.Fatalf("the plane was asked from cursors %v, want the second page asked from 1", asked)
	}
}
