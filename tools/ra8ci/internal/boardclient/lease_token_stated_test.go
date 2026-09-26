// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"testing"
)

// refusingServer fails the test if any request reaches it. Every case here is
// about a request that must never leave this process.
func refusingServer(t *testing.T) (*Client, func()) {
	t.Helper()
	return testClient(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("a request was sent for a token this client can see is incomplete: %s %s", r.Method, r.URL.Path)
		w.WriteHeader(http.StatusBadRequest)
	})
}

// partialTokens names each field a lease-bound request cannot be made without,
// each case a whole token missing exactly one of them.
func partialTokens(whole LeaseToken) map[string]LeaseToken {
	noBoard, noLease, noWaiter, noGeneration := whole, whole, whole, whole
	noBoard.BoardID = ""
	noLease.LeaseID = ""
	noWaiter.RequestID = ""
	noGeneration.Generation = 0
	unnamedBoard := whole
	unnamedBoard.BoardID = "ek ra8d2/../other"
	shortLease := whole
	shortLease.LeaseID = "not-an-id"
	return map[string]LeaseToken{
		"no board":           noBoard,
		"no lease":           noLease,
		"no waiter":          noWaiter,
		"no generation":      noGeneration,
		"unnameable board":   unnamedBoard,
		"lease is not an id": shortLease,
	}
}

func TestFinishingASegmentRefusesAnIncompleteTokenBeforeSendingAnything(t *testing.T) {
	whole := testToken(activeBoard(t))
	c, closeServer := refusingServer(t)
	defer closeServer()
	for name, token := range partialTokens(whole) {
		err := c.FinishSegment(context.Background(), token, testProofID, "segment-1", "completed")
		if !errors.Is(err, ErrInvalidRequest) {
			t.Errorf("%s: finish accepted the token: err=%v", name, err)
		}
	}
}

func TestBothEndsOfASegmentRefuseTheSameIncompleteToken(t *testing.T) {
	whole := testToken(activeBoard(t))
	c, closeServer := refusingServer(t)
	defer closeServer()
	for name, token := range partialTokens(whole) {
		_, begun := c.BeginSegment(context.Background(), token, testProofID, "flash", 25000000000, 0)
		finished := c.FinishSegment(context.Background(), token, testProofID, "segment-1", "completed")
		if !errors.Is(begun, ErrInvalidRequest) || !errors.Is(finished, ErrInvalidRequest) {
			t.Errorf("%s: the two ends of one operation disagree: begin=%v finish=%v", name, begun, finished)
		}
	}
}

func TestTheFinishDoorAppliesExactlyTheRuleTheReadDoorApplies(t *testing.T) {
	whole := testToken(activeBoard(t))
	c, closeServer := refusingServer(t)
	defer closeServer()
	for name, token := range partialTokens(whole) {
		if validLeaseToken(token) {
			t.Errorf("%s: the shared rule accepted an incomplete token", name)
		}
		if _, err := c.leaseStatus(context.Background(), token); !errors.Is(err, ErrInvalidRequest) {
			t.Errorf("%s: the read door accepted it: err=%v", name, err)
		}
	}
}

func TestAWholeTokenPassesBothDoors(t *testing.T) {
	state := activeBoard(t)
	whole := testToken(state)
	var finished bool
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/segments/segment-1/finish":
			finished = true
			jsonResponse(w, http.StatusOK, map[string]string{"outcome": "completed"})
		default:
			t.Errorf("unexpected route %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	defer closeServer()
	if !validLeaseToken(whole) {
		t.Fatalf("the token a granted lease produces is refused: %+v", whole)
	}
	if _, err := c.leaseStatus(context.Background(), whole); err != nil {
		t.Fatalf("the read door refused a whole token: %v", err)
	}
	if err := c.FinishSegment(context.Background(), whole, testProofID, "segment-1", "completed"); err != nil || !finished {
		t.Fatalf("the finish door refused a whole token: finished=%v err=%v", finished, err)
	}
}

func TestAWholeTokenIsStillJudgedOnWhatElseTheCallSays(t *testing.T) {
	whole := testToken(activeBoard(t))
	c, closeServer := refusingServer(t)
	defer closeServer()
	for name, call := range map[string]error{
		"attempt is not an id":            c.FinishSegment(context.Background(), whole, "not-an-id", "segment-1", "completed"),
		"no segment":                      c.FinishSegment(context.Background(), whole, testProofID, "", "completed"),
		"outcome is not one of the three": c.FinishSegment(context.Background(), whole, testProofID, "segment-1", "done"),
	} {
		if !errors.Is(call, ErrInvalidRequest) {
			t.Errorf("%s: accepted: err=%v", name, call)
		}
	}
}
