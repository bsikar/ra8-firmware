// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// An operator reading a transcript almost always reads it through something:
// a pager, head, a pipe into grep. That reader closing first is the ordinary
// end of the story, and the write that lands after it is the one place these
// commands can lose a page without a word. What is pinned here is that the
// loss is named, that it names which stream and which chunk, and that the
// command stops rather than draining the rest of the plane into a stream
// nobody is holding any more.

// closedStream hands back a stream that is already shut. Writing to it fails
// with an ordinary error rather than a signal, because the descriptor is a
// pipe of our own and never standard output itself.
func closedStream(t *testing.T) *os.File {
	t.Helper()
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := reader.Close(); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return writer
}

// spokenIntoAClosedStream runs a command with one standard stream shut and the
// other opened on the null device, so the failure under test is the only one
// in the run and the other stream never blocks on a reader we are not draining.
func spokenIntoAClosedStream(t *testing.T, broken string, run func() error) error {
	t.Helper()
	working, err := os.OpenFile(os.DevNull, os.O_WRONLY, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := working.Close(); err != nil {
			t.Fatal(err)
		}
	}()
	savedOut, savedErr := os.Stdout, os.Stderr
	if broken == "stderr" {
		os.Stdout, os.Stderr = working, closedStream(t)
	} else {
		os.Stdout, os.Stderr = closedStream(t), working
	}
	runErr := run()
	os.Stdout, os.Stderr = savedOut, savedErr
	return runErr
}

// The chunk that could not be written is named by its stream and its sequence.
// Without that an operator has a transcript with a hole in it and no way to
// say where, and the two streams fail independently: stderr going away does
// not mean stdout did.
func TestRunLogsNamesTheChunkAWriteCouldNotLand(t *testing.T) {
	for _, stream := range []string{"stdout", "stderr"} {
		t.Run(stream, func(t *testing.T) {
			servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/logs",
				store.LogPage{AttemptID: answeredRun, NextAfter: 2, Chunks: []store.LogRecord{
					logChunk(1, "stdout", "configuring\n", 1000),
					logChunk(2, "stderr", "warning: stub crypto\n", 2000),
				}}))

			err := spokenIntoAClosedStream(t, stream, func() error {
				return showRunLogs(context.Background(), []string{answeredRun, answeredRun})
			})
			if err == nil {
				t.Fatal("a chunk that could not be written was reported as a transcript read to its end")
			}
			wanted := "write stdout log chunk 1"
			if stream == "stderr" {
				wanted = "write stderr log chunk 2"
			}
			if !strings.Contains(err.Error(), wanted) {
				t.Fatalf("the refusal was %q, want it to name %q", err, wanted)
			}
		})
	}
}

// A lost write ends the read. Carrying on would spend the rest of the plane's
// pages on a stream nobody is holding, and would hand back the paging error
// from some later page instead of the write that actually broke.
func TestRunLogsStopsReadingOnceAWriteIsLost(t *testing.T) {
	plane := &pagedRunPlane{servingPath: "/v1/runs/" + answeredRun + "/logs", pages: []any{
		store.LogPage{AttemptID: answeredRun, NextAfter: 1, HasMore: true,
			Chunks: []store.LogRecord{logChunk(1, "stdout", "configuring\n", 1000)}},
		store.LogPage{AttemptID: answeredRun, NextAfter: 2,
			Chunks: []store.LogRecord{logChunk(2, "stdout", "building\n", 2000)}},
	}}
	servingRuns(t, plane.serve(t))

	err := spokenIntoAClosedStream(t, "stdout", func() error {
		return showRunLogs(context.Background(), []string{"--limit", "1", answeredRun, answeredRun})
	})
	if err == nil || !strings.Contains(err.Error(), "write stdout log chunk 1") {
		t.Fatalf("the refusal was %v, want the first chunk named", err)
	}
	plane.mu.Lock()
	asked := len(plane.asked)
	plane.mu.Unlock()
	if asked != 1 {
		t.Fatalf("the plane was asked for %d pages, want the read to stop at the lost write", asked)
	}
}

// The event log fails the same way and must say so. These lines are what a
// caller scripts against, so a page silently dropped reads downstream as an
// attempt that never reached the state it did.
func TestRunEventsReportsAnEventLineThatCouldNotBeWritten(t *testing.T) {
	happened := time.Now().UTC().Truncate(time.Second)
	servingRuns(t, atPath(t, "/v1/runs/"+answeredRun+"/events",
		store.RunEventPage{RunID: answeredRun, NextAfter: 1, Events: []store.RunEvent{{
			Sequence: 1, ID: answeredRun, Kind: "run_created",
			Data: json.RawMessage(`{"trigger":"manual"}`), HappenedAt: happened,
		}}}))

	err := spokenIntoAClosedStream(t, "stdout", func() error {
		return showRunEvents(context.Background(), []string{answeredRun})
	})
	if err == nil {
		t.Fatal("an event line that could not be written was reported as an event log read to its end")
	}
	if !strings.Contains(err.Error(), "write JSON output") {
		t.Fatalf("the refusal was %q, want it to name the write", err)
	}
}
