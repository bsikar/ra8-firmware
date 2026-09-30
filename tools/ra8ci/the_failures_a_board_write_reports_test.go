// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// the_cancel_and_the_plan_a_board_accepts_test.go pins what these two
// commands say when the plane answers them. This pins what they say when it
// does not. Both commands carry one named refusal and one generic wrapper,
// and only the named half had cases: an operator meeting the generic half was
// being told nothing this tree had ever checked.

// refusingRead answers the board read with a server failure, which is the
// shape of a plane that is up enough to accept the connection and not well
// enough to answer. Nothing is posted, because neither command gets that far.
func refusingRead(t *testing.T, status int) http.HandlerFunc {
	t.Helper()
	return func(writer http.ResponseWriter, request *http.Request) {
		if request.Method != http.MethodGet {
			t.Errorf("the command posted %s %s after a read it never got an answer to",
				request.Method, request.URL.Path)
		}
		writer.WriteHeader(status)
	}
}

// refusingWrite answers the read with state and then refuses the command
// itself. This is the harder case: the operator's ticket is really there, and
// the withdrawal really did not happen.
func refusingWrite(t *testing.T, state board.Snapshot, status int) (http.HandlerFunc, *int) {
	t.Helper()
	posts := 0
	return func(writer http.ResponseWriter, request *http.Request) {
		if request.Method == http.MethodGet {
			writer.Header().Set("Content-Type", "application/json")
			if err := json.NewEncoder(writer).Encode(state); err != nil {
				t.Errorf("the stand-in plane could not answer: %v", err)
			}
			return
		}
		posts++
		writer.WriteHeader(status)
	}, &posts
}

// A cancellation the plane would not answer is reported as a cancellation
// that did not happen, naming the request. The failure must not borrow the
// granted wording: "already granted" tells an operator the board is theirs
// and the request is spent, and a refused withdrawal means neither.
func TestBoardCancelReportsAWithdrawalThePlaneWouldNotAnswer(t *testing.T) {
	servingBoard(t, refusingRead(t, http.StatusInternalServerError))

	said, err := spoken(t, func() error {
		return boardCancelCommand(context.Background(), []string{pinnedBoard, pinnedRequest, pinnedLease})
	})
	if err == nil {
		t.Fatalf("a refused cancellation was reported as done; said %q", said)
	}
	if !strings.HasPrefix(err.Error(), "cancel board request "+pinnedRequest+": ") {
		t.Fatalf("refusal = %v; want the failed cancellation named with its request", err)
	}
	if strings.Contains(err.Error(), "already granted") {
		t.Fatalf("refusal = %v; want a failure, not a grant the plane never reported", err)
	}
	if said != "" {
		t.Fatalf("stdout = %q; want nothing said about a cancellation that did not happen", said)
	}
}

// The same wrapper, reached the other way: the read succeeds, the ticket is
// genuinely queued, and the withdrawal itself is refused. The operator has to
// learn their request is still in the queue, so the post is asserted to have
// actually been attempted rather than skipped.
func TestBoardCancelReportsAQueuedTicketThePlaneWouldNotWithdraw(t *testing.T) {
	queued := boardHoldingALease(strangersLease, strangersUser)
	queued.Queue = []board.Waiter{waiterInTheQueue(pinnedRequest, pinnedLease)}
	handler, posts := refusingWrite(t, queued, http.StatusInternalServerError)
	servingBoard(t, handler)

	said, err := spoken(t, func() error {
		return boardCancelCommand(context.Background(), []string{pinnedBoard, pinnedRequest, pinnedLease})
	})
	if err == nil {
		t.Fatalf("a refused withdrawal was reported as done; said %q", said)
	}
	if !strings.HasPrefix(err.Error(), "cancel board request "+pinnedRequest+": ") {
		t.Fatalf("refusal = %v; want the failed cancellation named with its request", err)
	}
	if *posts == 0 {
		t.Fatal("the withdrawal was never attempted, so the queue entry was reported on without being touched")
	}
	if said != "" {
		t.Fatalf("stdout = %q; want nothing said about a cancellation that did not happen", said)
	}
}

// A recovery start the plane would not answer is reported as a failed start,
// not as a board that is not waiting for one. The two readings send an
// operator in opposite directions: one is a board to leave alone, the other
// is a plane to chase.
func TestBoardRecoverReportsAStartThePlaneWouldNotAnswer(t *testing.T) {
	servingBoard(t, refusingRead(t, http.StatusInternalServerError))

	said, err := spoken(t, func() error {
		return boardRecoverCommand(context.Background(),
			[]string{pinnedBoard, "--plan", pinnedPlan, "--why", "the fixture looks wedged"})
	})
	if err == nil {
		t.Fatalf("a refused recovery start was reported as done; said %q", said)
	}
	if !strings.HasPrefix(err.Error(), "start board recovery: ") {
		t.Fatalf("refusal = %v; want the failed start named as a start", err)
	}
	if strings.Contains(err.Error(), "not waiting for a recovery plan") {
		t.Fatalf("refusal = %v; want a failure, not a phase the plane never reported", err)
	}
	if said != "" {
		t.Fatalf("stdout = %q; want no snapshot printed for a recovery that did not start", said)
	}
}
