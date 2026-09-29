// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

// Taking a board is the one command that both asks for something and then
// waits for it, and the waiting is where it can go wrong in ways an operator
// has to be told about honestly: a take the plane accepted but cannot show,
// a queue position that disappears while we wait, and a grant that arrives
// but cannot be written down privately. All three are pinned here alongside
// the ordinary grant, end to end against a stand-in plane.

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// takingPlane answers a take end to end. The request and lease identifiers are
// minted inside the client, so the plane learns them from the take it is sent
// and answers every later read in terms of them, which is what makes a grant
// for THIS ticket distinguishable from a board somebody else holds.
type takingPlane struct {
	mu         sync.Mutex
	requestID  string
	leaseID    string
	takeBody   map[string]any
	cancels    int
	answerTake string // "granted", "queued" or "elsewhere"
	afterTake  string // what later reads show: "granted", "queued" or "elsewhere"
}

func (p *takingPlane) board(kind string) board.Snapshot {
	switch kind {
	case "granted":
		return boardHoldingALease(p.leaseID, p.requestID)
	case "queued":
		waiting := boardHoldingALease(strangersLease, strangersUser)
		waiting.Queue = []board.Waiter{waiterInTheQueue(p.requestID, p.leaseID)}
		return waiting
	default:
		return boardHoldingALease(strangersLease, strangersUser)
	}
}

func (p *takingPlane) serve(t *testing.T) http.HandlerFunc {
	t.Helper()
	return func(writer http.ResponseWriter, request *http.Request) {
		p.mu.Lock()
		defer p.mu.Unlock()
		writer.Header().Set("Content-Type", "application/json")
		if request.Method == http.MethodGet {
			state := readyBoard(pinnedBoard)
			if p.requestID != "" {
				state = p.board(p.afterTake)
			}
			if err := json.NewEncoder(writer).Encode(state); err != nil {
				t.Errorf("the stand-in plane could not answer the read: %v", err)
			}
			return
		}
		body := map[string]any{}
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Errorf("the stand-in plane could not read what it was asked: %v", err)
		}
		if strings.HasSuffix(request.URL.Path, "/take") {
			p.requestID, _ = body["request_id"].(string)
			p.leaseID, _ = body["lease_id"].(string)
			p.takeBody = body
			if err := json.NewEncoder(writer).Encode(map[string]any{"snapshot": p.board(p.answerTake)}); err != nil {
				t.Errorf("the stand-in plane could not answer the take: %v", err)
			}
			return
		}
		p.cancels++
		if err := json.NewEncoder(writer).Encode(map[string]any{"snapshot": p.board("elsewhere")}); err != nil {
			t.Errorf("the stand-in plane could not answer the withdrawal: %v", err)
		}
	}
}

func TestBoardTakeSpeaksTheGrantAndSavesTheLeaseItWasGiven(t *testing.T) {
	privateConfigHome(t)
	plane := &takingPlane{answerTake: "granted", afterTake: "granted"}
	servingBoard(t, plane.serve(t))

	said, err := spoken(t, func() error {
		return boardTakeCommand(context.Background(),
			[]string{pinnedBoard, "--class", "human", "--why", "bringing up the bench", "--duration", "1h"})
	})
	if err != nil {
		t.Fatalf("take: %v", err)
	}
	answer := struct {
		Ticket boardclient.Ticket     `json:"ticket"`
		Lease  boardclient.LeaseToken `json:"lease"`
	}{}
	if err := json.Unmarshal([]byte(said), &answer); err != nil {
		t.Fatalf("stdout %q is not the JSON an operator is given: %v", said, err)
	}
	if answer.Ticket.RequestID != plane.requestID || answer.Lease.LeaseID != plane.leaseID ||
		answer.Lease.BoardID != pinnedBoard || answer.Lease.Generation != 3 {
		t.Fatalf("operator was shown ticket=%+v lease=%+v; want the ticket the plane granted",
			answer.Ticket, answer.Lease)
	}
	if plane.takeBody["class"] != "human" || plane.takeBody["why"] != "bringing up the bench" {
		t.Fatalf("take carried %v; want the class and reason asked for", plane.takeBody)
	}
	if seconds, ok := plane.takeBody["duration_seconds"].(float64); !ok || int64(seconds) != 3600 {
		t.Fatalf("take carried duration %v; want 3600 whole seconds", plane.takeBody["duration_seconds"])
	}

	directory, err := currentBoardLeaseDirectory()
	if err != nil {
		t.Fatal(err)
	}
	saved, err := readBoardLeaseToken(directory, pinnedBoard)
	if err != nil {
		t.Fatalf("the granted lease was not saved where the next command reads it: %v", err)
	}
	if saved.LeaseID != plane.leaseID || saved.RequestID != plane.requestID || saved.Generation != 3 {
		t.Fatalf("saved token = %+v; want the granted lease", saved)
	}
}

func TestBoardTakeCallsTheOutcomeAmbiguousWhenAnAcceptedTakeIsNotOnTheBoard(t *testing.T) {
	privateConfigHome(t)
	plane := &takingPlane{answerTake: "elsewhere", afterTake: "elsewhere"}
	servingBoard(t, plane.serve(t))

	said, err := spoken(t, func() error {
		return boardTakeCommand(context.Background(),
			[]string{pinnedBoard, "--why", "bringing up the bench", "--duration", "30m"})
	})
	if err == nil {
		t.Fatalf("a take the board never showed was reported as done; said %q", said)
	}
	if !strings.Contains(err.Error(), "outcome may be ambiguous") ||
		!strings.Contains(err.Error(), plane.requestID) || !strings.Contains(err.Error(), plane.leaseID) {
		t.Fatalf("refusal = %v; want both identifiers named so the request can be chased", err)
	}
	if said != "" {
		t.Fatalf("stdout = %q; want no lease spoken for a take that may not have landed", said)
	}
}

func TestBoardTakeReportsAQueuePositionThatDisappearsWhileItWaits(t *testing.T) {
	privateConfigHome(t)
	plane := &takingPlane{answerTake: "queued", afterTake: "elsewhere"}
	servingBoard(t, plane.serve(t))

	said, err := spoken(t, func() error {
		return boardTakeCommand(context.Background(),
			[]string{pinnedBoard, "--why", "bringing up the bench", "--duration", "1h"})
	})
	if err == nil {
		t.Fatalf("a vanished request was reported as granted; said %q", said)
	}
	if !strings.HasPrefix(err.Error(), "waiting for board request "+plane.requestID+":") {
		t.Fatalf("refusal = %v; want the wait named against this request", err)
	}
	if said != "" {
		t.Fatalf("stdout = %q; want no lease spoken for a grant that never came", said)
	}
}

func TestBoardTakeSpeaksTheGrantEvenWhenTheTokenCannotBeSavedPrivately(t *testing.T) {
	home := privateConfigHome(t)
	// A regular file where the lease store's directory belongs: the grant is
	// real, the private copy is impossible, and the operator must get both
	// facts rather than one of them.
	if err := os.WriteFile(filepath.Join(home, "ra8ci"), []byte("not a directory"), 0o600); err != nil {
		t.Fatal(err)
	}
	plane := &takingPlane{answerTake: "granted", afterTake: "granted"}
	servingBoard(t, plane.serve(t))

	said, err := spoken(t, func() error {
		return boardTakeCommand(context.Background(),
			[]string{pinnedBoard, "--why", "bringing up the bench", "--duration", "1h"})
	})
	if err == nil {
		t.Fatal("an unsaved lease token was reported as a clean grant")
	}
	if !strings.Contains(err.Error(), "token JSON was written to stdout but could not be saved privately") {
		t.Fatalf("refusal = %v; want the operator told the grant stands and the copy does not", err)
	}
	answer := struct {
		Lease boardclient.LeaseToken `json:"lease"`
	}{}
	if err := json.Unmarshal([]byte(said), &answer); err != nil {
		t.Fatalf("stdout %q is not the lease JSON the message promises: %v", said, err)
	}
	if answer.Lease.LeaseID != plane.leaseID || !answer.Lease.ExpiresAt.After(time.Now()) {
		t.Fatalf("spoken lease = %+v; want the live granted lease", answer.Lease)
	}
}
