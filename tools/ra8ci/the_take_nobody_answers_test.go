// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"sync"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// the_grant_a_take_waits_for_test.go pins a take that is answered. These are
// the two arms it does not reach: a request the client will not send at all,
// and a request that is sent and then cannot be taken back.

// A board name the client refuses is refused before anything is sent, and the
// bare refusal is what reaches the operator: the ambiguous-outcome wording
// belongs to a request that really was minted, and using it here would tell
// somebody a board might be held for them when nothing ever left this machine.
func TestBoardTakeRefusesABoardNameItWillNotSend(t *testing.T) {
	asked := 0
	servingBoard(t, func(writer http.ResponseWriter, request *http.Request) {
		asked++
		t.Errorf("the plane was asked %s %s about a board name the client refuses",
			request.Method, request.URL.Path)
		writer.WriteHeader(http.StatusInternalServerError)
	})

	for _, name := range []string{"", "ek ra8d2", "ek/ra8d2", "ek:ra8d2", strings.Repeat("b", 129)} {
		said, err := spoken(t, func() error {
			return boardTakeCommand(context.Background(),
				[]string{name, "--why", "bringing up the bench", "--duration", "1h"})
		})
		if err == nil {
			t.Fatalf("board name %q was sent; said %q", name, said)
		}
		if strings.Contains(err.Error(), "ambiguous") {
			t.Fatalf("board name %q: refusal = %v; want no outcome claimed for a request never minted", name, err)
		}
		if said != "" {
			t.Fatalf("board name %q: stdout = %q; want nothing said", name, said)
		}
	}
	if asked != 0 {
		t.Fatalf("the plane was asked %d times about a board name the client refuses", asked)
	}
}

// abandoningPlane accepts the take and then stops answering, which is the
// plane restarting between the queue and the grant. The ticket is real and
// on the board; this machine just cannot see it any more.
type abandoningPlane struct {
	mu        sync.Mutex
	requestID string
	leaseID   string
	cancels   int
}

func (p *abandoningPlane) serve(t *testing.T) http.HandlerFunc {
	t.Helper()
	return func(writer http.ResponseWriter, request *http.Request) {
		p.mu.Lock()
		defer p.mu.Unlock()
		if request.Method == http.MethodGet {
			if p.requestID != "" {
				writer.WriteHeader(http.StatusServiceUnavailable)
				return
			}
			writer.Header().Set("Content-Type", "application/json")
			if err := json.NewEncoder(writer).Encode(readyBoard(pinnedBoard)); err != nil {
				t.Errorf("the stand-in plane could not answer the read: %v", err)
			}
			return
		}
		body := map[string]any{}
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Errorf("the stand-in plane could not read what it was asked: %v", err)
		}
		if !strings.HasSuffix(request.URL.Path, "/take") {
			p.cancels++
			writer.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		p.requestID, _ = body["request_id"].(string)
		p.leaseID, _ = body["lease_id"].(string)
		queued := boardHoldingALease(strangersLease, strangersUser)
		queued.Queue = []board.Waiter{waiterInTheQueue(p.requestID, p.leaseID)}
		writer.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(writer).Encode(map[string]any{"snapshot": queued}); err != nil {
			t.Errorf("the stand-in plane could not answer the take: %v", err)
		}
	}
}

// A take that is queued and then cannot be withdrawn leaves the board at
// risk: the request may still be sitting there, and may even be granted to
// this machine while nobody is watching for it. The operator is told both
// halves, because "the wait failed" alone would read as nothing having
// happened, and they would queue a second request behind their own.
func TestBoardTakeSaysARequestMayStillBeQueuedWhenTheWithdrawalAlsoFails(t *testing.T) {
	privateConfigHome(t)
	plane := &abandoningPlane{}
	servingBoard(t, plane.serve(t))

	said, err := spoken(t, func() error {
		return boardTakeCommand(context.Background(),
			[]string{pinnedBoard, "--why", "bringing up the bench", "--duration", "1h"})
	})
	if err == nil {
		t.Fatalf("a take nobody could confirm was reported as a grant; said %q", said)
	}
	plane.mu.Lock()
	request, cancels := plane.requestID, plane.cancels
	plane.mu.Unlock()
	if request == "" {
		t.Fatal("the take never reached the plane, so this is not the arm under test")
	}
	if !strings.HasPrefix(err.Error(), "board request "+request+" is still queued or granted;") {
		t.Fatalf("refusal = %v; want the request named as possibly still queued", err)
	}
	for _, half := range []string{"wait failed:", "cancel failed:"} {
		if !strings.Contains(err.Error(), half) {
			t.Fatalf("refusal = %v; want both halves reported, missing %q", err, half)
		}
	}
	// The withdrawal never gets as far as a post: it has to read the board
	// first, and that read is the one that is failing. That is worth pinning,
	// because it is why the message cannot promise the request was dropped.
	if cancels != 0 {
		t.Fatalf("the plane was posted %d withdrawals it could not have answered", cancels)
	}
	if said != "" {
		t.Fatalf("stdout = %q; want no ticket printed for a lease this machine does not hold", said)
	}
}
