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
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The board read commands need no database and no bench: the client loads its
// material from disk and only the request itself wants a plane, so a
// mutual-TLS stand-in answers them end to end. What is pinned here is what an
// operator actually gets back, and what they are told when the answer is not
// about the board they asked about.

// servingBoard answers board reads with whatever the case supplies and points
// the environment at itself. The existing servingReport only answers the slow
// report, so this is its board-shaped sibling rather than an extension of it.
func servingBoard(t *testing.T, handler http.HandlerFunc) {
	t.Helper()
	material := mintReportMaterial(t)
	servingReport(t, material, handler)
}

// readyBoard is the smallest snapshot board.Validate accepts: an identifier
// and a phase, no lease, no queue.
func readyBoard(boardID string) board.Snapshot {
	return board.Snapshot{BoardID: boardID, Phase: board.Ready}
}

// answering serves one JSON body at one path and 404s everything else, so a
// command asking the wrong path fails loudly rather than reading a body meant
// for another endpoint.
func answering(t *testing.T, path string, body any) http.HandlerFunc {
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

// spoken captures what a command writes to stdout, which is the operator's
// whole answer for these read commands.
func spoken(t *testing.T, run func() error) (string, error) {
	t.Helper()
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	saved := os.Stdout
	os.Stdout = writer
	runErr := run()
	os.Stdout = saved
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	spoke, err := io.ReadAll(reader)
	if err != nil {
		t.Fatal(err)
	}
	if err := reader.Close(); err != nil {
		t.Fatal(err)
	}
	return string(spoke), runErr
}

func TestBoardStatusSpeaksTheSnapshotThePlaneAnswered(t *testing.T) {
	snapshot := readyBoard("bench-1")
	snapshot.Generation = 7
	snapshot.Version = 3
	servingBoard(t, answering(t, "/v1/boards/bench-1", snapshot))

	spoke, err := spoken(t, func() error {
		return boardStatusCommand(context.Background(), []string{"bench-1"})
	})
	if err != nil {
		t.Fatalf("a served board was refused: %v", err)
	}
	var read board.Snapshot
	if err := json.Unmarshal([]byte(spoke), &read); err != nil {
		t.Fatalf("stdout was not a snapshot: %v (%q)", err, spoke)
	}
	if read.BoardID != "bench-1" || read.Generation != 7 || read.Version != 3 {
		t.Errorf("the operator was told %+v", read)
	}
}

// An answer about a different board is not an answer about this one, however
// well formed it is. Reading it back would tell an operator the wrong board is
// free.
func TestBoardStatusRefusesAnAnswerAboutAnotherBoard(t *testing.T) {
	servingBoard(t, answering(t, "/v1/boards/bench-1", readyBoard("bench-2")))

	spoke, err := spoken(t, func() error {
		return boardStatusCommand(context.Background(), []string{"bench-1"})
	})
	if err == nil {
		t.Fatalf("an answer about another board was accepted: %q", spoke)
	}
	if spoke != "" {
		t.Errorf("a refused read still spoke %q", spoke)
	}
}

func TestBoardStatusCarriesWhatThePlaneRefused(t *testing.T) {
	servingBoard(t, func(writer http.ResponseWriter, _ *http.Request) {
		writer.WriteHeader(http.StatusServiceUnavailable)
	})

	if _, err := spoken(t, func() error {
		return boardStatusCommand(context.Background(), []string{"bench-1"})
	}); err == nil {
		t.Fatal("an unavailable plane read as a board")
	}
}

func TestBoardLivenessSpeaksTheSnapshotAndTheReport(t *testing.T) {
	servingBoard(t, answering(t, "/v1/boards/bench-1/liveness", map[string]any{
		"snapshot": readyBoard("bench-1"),
		"liveness": map[string]any{
			"held": false, "lease_id": "", "holder": "", "last_seen_at": nil,
			"beat": false, "silence_seconds": 0, "interval_seconds": 60,
			"next_beat_by": nil, "overdue": false, "expires_at": nil,
			"explain": "no lease is held",
		},
	}))

	spoke, err := spoken(t, func() error {
		return boardLivenessCommand(context.Background(), []string{"bench-1"})
	})
	if err != nil {
		t.Fatalf("a served liveness report was refused: %v", err)
	}
	var read struct {
		Snapshot board.Snapshot    `json:"snapshot"`
		Liveness boardLivenessLine `json:"liveness"`
	}
	if err := json.Unmarshal([]byte(spoke), &read); err != nil {
		t.Fatalf("stdout was not a liveness answer: %v (%q)", err, spoke)
	}
	if read.Snapshot.BoardID != "bench-1" {
		t.Errorf("the snapshot reads %+v", read.Snapshot)
	}
	if read.Liveness.Held {
		t.Errorf("an unheld board read as held: %+v", read.Liveness)
	}
	if !strings.Contains(spoke, "no lease is held") {
		t.Errorf("the explanation did not reach the operator: %q", spoke)
	}
}

// A liveness report naming a lease the snapshot does not record is about some
// other lease, so it is refused rather than read back as this board's health.
func TestBoardLivenessRefusesAReportAboutALeaseTheBoardDoesNotRecord(t *testing.T) {
	servingBoard(t, answering(t, "/v1/boards/bench-1/liveness", map[string]any{
		"snapshot": readyBoard("bench-1"),
		"liveness": map[string]any{
			"held": true, "lease_id": "lease-404", "holder": "runner-3",
			"last_seen_at": nil, "beat": true, "silence_seconds": 1,
			"interval_seconds": 60, "next_beat_by": nil, "overdue": false,
			"expires_at": nil, "explain": "held",
		},
	}))

	spoke, err := spoken(t, func() error {
		return boardLivenessCommand(context.Background(), []string{"bench-1"})
	})
	if err == nil {
		t.Fatalf("a report about another lease was accepted: %q", spoke)
	}
	if !strings.HasPrefix(err.Error(), "read board liveness: ") {
		t.Errorf("err=%q; want the liveness read to name itself", err)
	}
}

// Both commands refuse a board identifier they cannot send before any request
// is spent, and say what they wanted instead.
func TestTheBoardReadCommandsRefuseAnUnusableIdentifier(t *testing.T) {
	for _, testCase := range []struct {
		name string
		args []string
		want string
	}{
		{"liveness with no board", nil, "usage: ra8ci board liveness <board-id>"},
		{"liveness with two boards", []string{"bench-1", "bench-2"}, "usage: ra8ci board liveness <board-id>"},
		{"liveness with a path in the name", []string{"bench/1"}, "usage: ra8ci board liveness <board-id>"},
		{"liveness with a space in the name", []string{"bench 1"}, "usage: ra8ci board liveness <board-id>"},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			err := boardLivenessCommand(context.Background(), testCase.args)
			if err == nil || err.Error() != testCase.want {
				t.Fatalf("err=%v; want %q", err, testCase.want)
			}
		})
	}
	if err := boardStatusCommand(context.Background(), nil); err == nil {
		t.Error("status with no board was accepted")
	}
}

// The wire form is closed: a field the client does not declare is a plane
// speaking a protocol this build does not know, and reading the rest of the
// body anyway would let a newer server quietly change what a number means.
func TestBoardLivenessRefusesABodyCarryingAnUndeclaredField(t *testing.T) {
	servingBoard(t, answering(t, "/v1/boards/bench-1/liveness", map[string]any{
		"snapshot": readyBoard("bench-1"),
		"liveness": map[string]any{
			"held": false, "lease_id": "", "holder": "", "last_seen_at": nil,
			"beat": false, "silence_seconds": 0, "interval_seconds": 60,
			"next_beat_by": nil, "overdue": false, "expires_at": nil,
			"explain": "no lease is held", "as_of": "2026-09-29T00:00:00Z",
		},
	}))

	spoke, err := spoken(t, func() error {
		return boardLivenessCommand(context.Background(), []string{"bench-1"})
	})
	if err == nil {
		t.Fatalf("an undeclared field was accepted: %q", spoke)
	}
	if !strings.Contains(err.Error(), "as_of") {
		t.Errorf("err=%q; want the undeclared field named", err)
	}
}
