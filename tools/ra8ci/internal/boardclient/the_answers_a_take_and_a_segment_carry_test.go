// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A take and a segment are the two asks this client makes that the server
// answers with a document rather than an acknowledgement, so both hold the
// answer to the ask before any of it reaches a caller. These pin the answers
// this client refuses to believe, and the ticket it hands back anyway.

// takenBoard serves an empty board, then enqueues whatever waiter the take
// actually sent, so the answer is built from the request rather than assumed.
// refuseFirst answers the first take with a version conflict.
func takenBoard(t *testing.T, refuseFirst bool) (*Client, *atomic.Int64, func()) {
	t.Helper()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	var takes atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		if takes.Add(1) == 1 && refuseFirst {
			jsonResponse(w, http.StatusConflict, map[string]any{
				"code": "conflict", "detail": "expected version is stale", "retryable": true,
			})
			return
		}
		var ask struct {
			RequestID       string `json:"request_id"`
			LeaseID         string `json:"lease_id"`
			Why             string `json:"why"`
			DurationSeconds int64  `json:"duration_seconds"`
		}
		if err := json.NewDecoder(r.Body).Decode(&ask); err != nil {
			t.Errorf("take body: %v", err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		state = transition(t, state, board.Enqueue{Actor: "agent", Waiter: board.Waiter{
			ID: ask.RequestID, LeaseID: ask.LeaseID, Holder: "agent", Class: board.ClassAI,
			Reason: ask.Why, Duration: time.Duration(ask.DurationSeconds) * time.Second,
		}})
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
	})
	return client, &takes, done
}

// An unnamed class has no lifetime ceiling, which is what makes every
// duration refused for one rather than defaulting to the longest.
func TestAnUnnamedClassIsAllowedNoTime(t *testing.T) {
	if limit := classDurationLimit(board.ClassHuman + 1); limit != 0 {
		t.Fatalf("an unnamed class was allowed %v", limit)
	}
	if name := takeClassName(board.ClassHuman + 1); name != "" {
		t.Fatalf("an unnamed class was named %q", name)
	}
}

// The ticket comes back even when the take did not land, because its
// identifiers are the only way a caller can reconcile or cancel an
// ambiguous submission. Losing them loses the waiter.
func TestRequestTakeHandsBackTheTicketItCouldNotSubmit(t *testing.T) {
	client, takes, done := takenBoard(t, false)
	done()

	ticket, err := client.RequestTake(context.Background(), "ek-ra8d2", board.ClassAI, "HIL", time.Minute)
	if err == nil {
		t.Fatal("an unreachable server accepted a take")
	}
	if !store.ValidID(ticket.RequestID) || !store.ValidID(ticket.LeaseID) || ticket.BoardID != "ek-ra8d2" {
		t.Fatalf("the ticket for an ambiguous take is unusable: %+v", ticket)
	}
	if takes.Load() != 0 {
		t.Fatal("a take reached a closed server")
	}
}

// A server that accepts a take and then describes a board without it is not
// describing this board. Believing it would leave the caller waiting for a
// grant that nothing is queued for.
func TestRequestTakeRefusesAnAcceptedTakeItCannotSee(t *testing.T) {
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			jsonResponse(w, http.StatusOK, commandResponse{Snapshot: state})
			return
		}
		jsonResponse(w, http.StatusOK, state)
	})
	defer done()

	ticket, err := client.RequestTake(context.Background(), "ek-ra8d2", board.ClassAI, "HIL", time.Minute)
	if !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("an invisible take was accepted: %v", err)
	}
	if !store.ValidID(ticket.RequestID) {
		t.Fatalf("the ticket was lost with the answer: %+v", ticket)
	}
}

// A version conflict on a take means another writer moved the board, so the
// same ticket is re-read and re-sent. Minting a second ticket would queue
// this caller twice.
func TestRequestTakeRetriesAVersionConflictWithTheSameTicket(t *testing.T) {
	client, takes, done := takenBoard(t, true)
	defer done()

	ticket, err := client.RequestTake(context.Background(), "ek-ra8d2", board.ClassAI, "HIL", time.Minute)
	if err != nil {
		t.Fatalf("a version conflict was not retried: %v", err)
	}
	if takes.Load() != 2 {
		t.Fatalf("%d takes sent, expected a refused one and a retry", takes.Load())
	}
	snapshot, err := client.Status(context.Background(), "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	if !visibleTicket(snapshot, ticket) {
		t.Fatalf("the retried take queued something else: %+v", snapshot.Queue)
	}
	if len(snapshot.Queue) != 0 || snapshot.Lease == nil {
		t.Fatalf("the take was submitted twice: queue=%d lease=%+v", len(snapshot.Queue), snapshot.Lease)
	}
}

// The segment check is a read of server state, so a token the board does not
// know is refused there rather than turned into permission.
func TestCanStartSegmentRefusesATokenTheBoardDoesNotHold(t *testing.T) {
	state := activeBoard(t)
	client, _, done := countedBoard(t, state)
	defer done()

	stale := testToken(state)
	stale.LeaseID = testProofID
	if err := client.CanStartSegment(context.Background(), stale, time.Second, time.Second); !errors.Is(err, ErrStaleLease) {
		t.Fatalf("another lease was cleared to start a segment: %v", err)
	}
}

func TestBeginSegmentRefusesAnAskItCannotMake(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()
	token := testToken(state)

	for name, ask := range map[string]struct {
		attemptID      string
		key            string
		bound          time.Duration
		recoveryMargin time.Duration
	}{
		"an attempt ID that is not one": {"attempt-1", "flash", time.Second, time.Second},
		"no key":                        {testProofID, "", time.Second, time.Second},
		"no bound":                      {testProofID, "flash", 0, time.Second},
		"a bound finer than the wire":   {testProofID, "flash", time.Millisecond + 1, time.Second},
		"a negative recovery margin":    {testProofID, "flash", time.Second, -time.Millisecond},
		"a margin finer than the wire":  {testProofID, "flash", time.Second, time.Millisecond + 1},
	} {
		_, err := client.BeginSegment(context.Background(), token, ask.attemptID, ask.key, ask.bound, ask.recoveryMargin)
		if !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused segments still reached the server", commands.Load())
	}
}

// A refused begin is handed back as it happened. A segment the caller
// believes it holds is one it will try to finish against hardware.
func TestBeginSegmentHandsBackARefusedBegin(t *testing.T) {
	state := activeBoard(t)
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			jsonResponse(w, http.StatusServiceUnavailable, map[string]any{
				"code": "unavailable", "detail": "board service is draining", "retryable": true,
			})
			return
		}
		jsonResponse(w, http.StatusOK, state)
	})
	defer done()

	segment, err := client.BeginSegment(context.Background(), testToken(state), testProofID, "flash", time.Second, time.Second)
	if err == nil {
		t.Fatal("a refused begin was answered with a segment")
	}
	if segment.ID != "" {
		t.Fatalf("a refused begin still carried a segment: %+v", segment)
	}
}

// The identifier in this answer is the one the agent unwinds with on a path
// that does no read of its own, so an answer about another operation is
// refused here rather than carried into that write.
func TestBeginSegmentRefusesAnAnswerAboutAnotherOperation(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	sound := answeredSegment(t, token, testProofID, "flash")

	for name, mutate := range map[string]func(store.BoardSegment) store.BoardSegment{
		"an identifier that is not one": func(s store.BoardSegment) store.BoardSegment { s.ID = "segment-1"; return s },
		"another board":                 func(s store.BoardSegment) store.BoardSegment { s.BoardID = "ek-ra8m1"; return s },
		"another lease":                 func(s store.BoardSegment) store.BoardSegment { s.LeaseID = testProofID; return s },
		"another generation":            func(s store.BoardSegment) store.BoardSegment { s.Generation = token.Generation + 1; return s },
		"another attempt":               func(s store.BoardSegment) store.BoardSegment { s.AttemptID = testRequestID; return s },
		"another key":                   func(s store.BoardSegment) store.BoardSegment { s.Key = "restore"; return s },
		"no start":                      func(s store.BoardSegment) store.BoardSegment { s.StartedAt = time.Time{}; return s },
		"no deadline":                   func(s store.BoardSegment) store.BoardSegment { s.DeadlineAt = time.Time{}; return s },
		"a deadline at its start":       func(s store.BoardSegment) store.BoardSegment { s.DeadlineAt = s.StartedAt; return s },
	} {
		answer := mutate(sound)
		client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
			if r.Method == http.MethodPost {
				jsonResponse(w, http.StatusOK, answer)
				return
			}
			jsonResponse(w, http.StatusOK, state)
		})
		segment, err := client.BeginSegment(context.Background(), token, testProofID, "flash", time.Second, time.Second)
		if !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
		if segment.ID != "" {
			t.Fatalf("%s still carried a segment: %+v", name, segment)
		}
		done()
	}
}
