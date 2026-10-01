// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

// A board cancellation and a board recovery both read the board first and then
// decide whether to ask for anything at all. The decision is the whole point of
// each command: a cancellation that arrives after the grant must not release a
// board somebody is using, and a recovery plan must not be handed to a board
// that is working fine. Both are pinned here end to end against a stand-in
// plane, so what is checked is what an operator is told and what the plane is
// actually asked for, not the shape of an argument list.

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

const (
	pinnedBoard    = "ek-ra8d2"
	pinnedRequest  = "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a01"
	pinnedLease    = "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a02"
	pinnedPlan     = "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a03"
	strangersLease = "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a04"
	strangersUser  = "0193a7b1-2c3d-7e4f-8a9b-0c1d2e3f4a05"
)

// boardHoldingALease is the smallest snapshot board.Validate accepts for a live
// phase: the lease's generation is the board's, the agent has installed it, and
// the deadline is exactly the duration that was granted.
func boardHoldingALease(leaseID, waiterID string) board.Snapshot {
	granted := time.Now().UTC().Add(-10 * time.Minute).Truncate(time.Second)
	return board.Snapshot{BoardID: pinnedBoard, Phase: board.Active, Generation: 3,
		AgentHighWater: 3, Version: 8, NextSequence: 4,
		Lease: &board.Lease{ID: leaseID, WaiterID: waiterID, Holder: "brighton",
			Class: board.ClassHuman, Reason: "bringing up the bench", Generation: 3,
			GrantedAt: granted, ExpiresAt: granted.Add(time.Hour),
			RequestedDuration: time.Hour, DeadlineVersion: 1}}
}

// waiterInTheQueue is a queued request for the same board, distinct from
// whatever lease is retained so board.Validate does not read it as a collision.
func waiterInTheQueue(requestID, leaseID string) board.Waiter {
	return board.Waiter{ID: requestID, LeaseID: leaseID, Holder: "brighton",
		Class: board.ClassHuman, Reason: "waiting for the bench", Duration: time.Hour,
		Sequence: 2, QueuedAt: time.Now().UTC().Add(-time.Minute)}
}

// askedPlane answers the board read with state and every POST with after,
// recording the path and body of each POST it is sent.
type askedPlane struct {
	posts  []string
	bodies []map[string]any
}

func (p *askedPlane) serve(t *testing.T, state board.Snapshot, after board.Snapshot) http.HandlerFunc {
	t.Helper()
	return func(writer http.ResponseWriter, request *http.Request) {
		writer.Header().Set("Content-Type", "application/json")
		if request.Method == http.MethodGet {
			if request.URL.Path != "/v1/boards/"+pinnedBoard {
				writer.WriteHeader(http.StatusNotFound)
				return
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
		p.posts = append(p.posts, request.URL.Path)
		p.bodies = append(p.bodies, body)
		if err := json.NewEncoder(writer).Encode(map[string]any{"snapshot": after}); err != nil {
			t.Errorf("the stand-in plane could not answer the command: %v", err)
		}
	}
}

func TestBoardCancelWillNotReleaseARequestTheBoardAlreadyGranted(t *testing.T) {
	held := boardHoldingALease(pinnedLease, pinnedRequest)
	plane := &askedPlane{}
	servingBoard(t, plane.serve(t, held, held))

	said, err := spoken(t, func() error {
		return boardCancelCommand(context.Background(), []string{pinnedBoard, pinnedRequest, pinnedLease})
	})
	if err == nil {
		t.Fatalf("a granted request was cancelled; said %q", said)
	}
	if !strings.Contains(err.Error(), "was already granted") ||
		!strings.Contains(err.Error(), pinnedRequest) {
		t.Fatalf("refusal = %v; want the request named as already granted", err)
	}
	if said != "" {
		t.Fatalf("stdout = %q; want nothing said about a cancellation that did not happen", said)
	}
	if len(plane.posts) != 0 {
		t.Fatalf("plane was asked %v; want a granted lease recognised from the read alone", plane.posts)
	}
}

func TestBoardCancelAnswersCancelledForATicketTheBoardNoLongerCarries(t *testing.T) {
	plane := &askedPlane{}
	gone := readyBoard(pinnedBoard)
	servingBoard(t, plane.serve(t, gone, gone))

	said, err := spoken(t, func() error {
		return boardCancelCommand(context.Background(), []string{pinnedBoard, pinnedRequest, pinnedLease})
	})
	if err != nil {
		t.Fatalf("cancel: %v", err)
	}
	answer := map[string]any{}
	if err := json.Unmarshal([]byte(said), &answer); err != nil {
		t.Fatalf("stdout %q is not the JSON an operator is given: %v", said, err)
	}
	if answer["board_id"] != pinnedBoard || answer["request_id"] != pinnedRequest ||
		answer["cancelled"] != true {
		t.Fatalf("answer = %v; want the board and request named and cancelled true", answer)
	}
	if len(plane.posts) != 0 {
		t.Fatalf("plane was asked %v; want nothing asked of a board that never had the ticket", plane.posts)
	}
}

func TestBoardCancelWithdrawsAQueuedTicketAtTheVersionItRead(t *testing.T) {
	queued := boardHoldingALease(strangersLease, strangersUser)
	queued.Queue = []board.Waiter{waiterInTheQueue(pinnedRequest, pinnedLease)}
	withdrawn := boardHoldingALease(strangersLease, strangersUser)
	withdrawn.Version = 9
	plane := &askedPlane{}
	servingBoard(t, plane.serve(t, queued, withdrawn))

	said, err := spoken(t, func() error {
		return boardCancelCommand(context.Background(), []string{pinnedBoard, pinnedRequest, pinnedLease})
	})
	if err != nil {
		t.Fatalf("cancel: %v", err)
	}
	if len(plane.posts) != 1 ||
		plane.posts[0] != "/v1/boards/"+pinnedBoard+"/waiters/"+pinnedRequest+"/cancel" {
		t.Fatalf("plane was asked %v; want one withdrawal of this waiter", plane.posts)
	}
	if version, ok := plane.bodies[0]["expected_version"].(float64); !ok || uint64(version) != queued.Version {
		t.Fatalf("withdrawal carried %v; want the version %d the read returned",
			plane.bodies[0]["expected_version"], queued.Version)
	}
	answer := map[string]any{}
	if err := json.Unmarshal([]byte(said), &answer); err != nil || answer["cancelled"] != true {
		t.Fatalf("stdout = %q; want the cancellation spoken as JSON", said)
	}
}

func TestBoardRecoverTellsAnOperatorABoardIsNotWaitingForAPlan(t *testing.T) {
	working := boardHoldingALease(strangersLease, strangersUser)
	plane := &askedPlane{}
	servingBoard(t, plane.serve(t, working, working))

	said, err := spoken(t, func() error {
		return boardRecoverCommand(context.Background(),
			[]string{pinnedBoard, "--plan", pinnedPlan, "--why", "swapped the debug probe"})
	})
	if err == nil {
		t.Fatalf("a working board accepted a recovery plan; said %q", said)
	}
	if err.Error() != "board "+pinnedBoard+" is not waiting for a recovery plan" {
		t.Fatalf("refusal = %v; want the board named in plain words", err)
	}
	if said != "" || len(plane.posts) != 0 {
		t.Fatalf("stdout = %q, plane asked %v; want neither", said, plane.posts)
	}
}

func TestBoardRecoverHandsThePlanToABoardWaitingForOne(t *testing.T) {
	for _, waiting := range []board.Phase{board.RecoveryRequired, board.Quarantined} {
		t.Run(string(waiting), func(t *testing.T) {
			stuck := board.Snapshot{BoardID: pinnedBoard, Phase: waiting, Version: 11}
			recovering := board.Snapshot{BoardID: pinnedBoard, Phase: board.Recovering, Version: 12}
			plane := &askedPlane{}
			servingBoard(t, plane.serve(t, stuck, recovering))

			said, err := spoken(t, func() error {
				return boardRecoverCommand(context.Background(),
					[]string{pinnedBoard, "--plan", pinnedPlan, "--why", "swapped the debug probe"})
			})
			if err != nil {
				t.Fatalf("recover: %v", err)
			}
			if len(plane.posts) != 1 || plane.posts[0] != "/v1/boards/"+pinnedBoard+"/recovery/start" {
				t.Fatalf("plane was asked %v; want one recovery start", plane.posts)
			}
			asked := plane.bodies[0]
			if asked["plan_id"] != pinnedPlan || asked["why"] != "swapped the debug probe" {
				t.Fatalf("recovery carried %v; want the reviewed plan and the reason", asked)
			}
			if version, ok := asked["expected_version"].(float64); !ok || uint64(version) != stuck.Version {
				t.Fatalf("recovery carried version %v; want %d", asked["expected_version"], stuck.Version)
			}
			answered := board.Snapshot{}
			if err := json.Unmarshal([]byte(said), &answered); err != nil {
				t.Fatalf("stdout %q is not a snapshot: %v", said, err)
			}
			if answered.Phase != board.Recovering || answered.Version != 12 {
				t.Fatalf("operator was shown %+v; want the recovering board", answered)
			}
		})
	}
}
