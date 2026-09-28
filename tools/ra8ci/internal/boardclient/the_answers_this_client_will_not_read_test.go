// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"sync/atomic"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Every call this client makes goes out through one request path, so what
// that path refuses to send, and refuses to read back, decides what the
// whole client can be told by a server. A half-read or half-trusted answer
// here becomes a board decision everywhere above it.

// A client assembled without its transport never reaches the network. New
// is the only thing that builds a usable one, so a zero value is a
// programming error, and it is answered as bad configuration rather than
// panicking in the middle of somebody's lease.
func TestAClientWithoutATransportNeverReachesTheNetwork(t *testing.T) {
	for name, client := range map[string]*Client{
		"no client at all":  nil,
		"a zero value":      {},
		"no HTTP client":    {base: &testURL},
		"no server address": {http: http.DefaultClient},
	} {
		if _, err := client.Status(context.Background(), "ek-ra8d2"); !errors.Is(err, ErrInvalidConfig) {
			t.Fatalf("%s = %v, want the request refused as bad configuration", name, err)
		}
	}
}

// A call with no context is refused before anything is sent. The server
// never sees it, so nothing has to be reconciled afterwards.
func TestARequestWithoutAContextIsNeverSent(t *testing.T) {
	var served atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		served.Add(1)
		jsonResponse(w, http.StatusOK, board.Snapshot{})
	})
	t.Cleanup(done)

	var missing context.Context
	if _, err := client.Status(missing, "ek-ra8d2"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("answered %v, want a request with no context refused", err)
	}
	if got := served.Load(); got != 0 {
		t.Fatalf("server was asked %d times for a request that should never have been sent", got)
	}
}

// An answer that stops early is not a short answer, it is an unknown one.
// Reading the prefix as a snapshot would report a board state the server
// never finished stating.
func TestAnAnswerThatStopsEarlyIsNotReadAsABoard(t *testing.T) {
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		hijacker, ok := w.(http.Hijacker)
		if !ok {
			t.Error("the test server cannot be made to hang up mid-answer")
			return
		}
		connection, buffered, err := hijacker.Hijack()
		if err != nil {
			t.Error(err)
			return
		}
		// A length the body never reaches: the answer is cut off.
		_, _ = buffered.WriteString("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
			"Content-Length: 4096\r\n\r\n{\"board_id\":\"ek-ra8")
		_ = buffered.Flush()
		_ = connection.Close()
	})
	t.Cleanup(done)

	snapshot, err := client.Status(context.Background(), "ek-ra8d2")
	if err == nil {
		t.Fatalf("a truncated answer was read as a board: %+v", snapshot)
	}
	if snapshot.BoardID != "" {
		t.Fatalf("a truncated answer left board state behind: %+v", snapshot)
	}
}

// One request is answered about one board. A second document in the same
// answer means the server is describing something this caller never asked
// about, and taking the first and discarding the rest would hide that.
func TestASecondDocumentInOneAnswerIsRefused(t *testing.T) {
	state := activeBoard(t)
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		writeJSON(t, w, state)
		writeJSON(t, w, state)
	})
	t.Cleanup(done)

	if _, err := client.Status(context.Background(), "ek-ra8d2"); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("answered %v, want a second document in one answer refused", err)
	}
}

// testURL is a parsed address a zero-value client can carry without a
// transport behind it.
var testURL = url.URL{Scheme: "https", Host: "localhost"}

func writeJSON(t *testing.T, w http.ResponseWriter, value any) {
	t.Helper()
	if err := json.NewEncoder(w).Encode(value); err != nil {
		t.Error(err)
	}
}
