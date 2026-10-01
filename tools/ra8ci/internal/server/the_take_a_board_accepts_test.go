// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The take door is where an operator, a CI job or an agent asks for the
// board. board_test.go holds its happy path for a human and the three
// untrusted-body refusals; this takes the argument table, which is the part
// that decides how long a lease can be asked for and by whom.

// tookTheBoard asks for the board and reads back what the plane answered.
func tookTheBoard(t *testing.T, f *fakeBoardStore, body string) answered {
	t.Helper()
	response := httptest.NewRecorder()
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(response,
		boardTestRequest("POST", "/v1/boards/ek-ra8d2/take", body))

	result := answered{status: response.Code}
	if response.Body.Len() > 0 {
		if err := json.Unmarshal(response.Body.Bytes(), &result.body); err != nil {
			t.Fatalf("the take door answered a body that is not JSON: %q", response.Body.String())
		}
	}
	return result
}

func aTake(requestID, leaseID, class, why string, seconds int64) string {
	return `{"expected_version":3,"request_id":"` + requestID + `","lease_id":"` + leaseID +
		`","class":"` + class + `","why":"` + why + `","duration_seconds":` +
		strconv.FormatInt(seconds, 10) + `}`
}

// All three declared classes are taken, and each reaches the store as the
// class the board reasons about rather than the word the caller sent.
func TestTheTakeDoorKnowsThreeClasses(t *testing.T) {
	for name, class := range map[string]board.Class{
		"human": board.ClassHuman,
		"ci":    board.ClassCI,
		"agent": board.ClassAI,
	} {
		f := &fakeBoardStore{}
		result := tookTheBoard(t, f, aTake(boardTestRequestID, boardTestLeaseID, name, "taking the board", 30))
		if result.status != http.StatusOK {
			t.Fatalf("%q answered %d %+v", name, result.status, result.body)
		}
		enqueue, ok := f.command.(board.Enqueue)
		if !ok {
			t.Fatalf("%q reached the store as %#v", name, f.command)
		}
		if enqueue.Waiter.Class != class {
			t.Fatalf("%q reached the store as class %v, want %v", name, enqueue.Waiter.Class, class)
		}
	}
}

// TestTheTakeDoorJudgesTheLeaseItIsAskedFor walks the argument table. The
// store counts what it was asked, so each refusal is also proof the request
// never became a queued waiter.
func TestTheTakeDoorJudgesTheLeaseItIsAskedFor(t *testing.T) {
	const eightHours = int64(8 * 60 * 60)

	for name, body := range map[string]string{
		"a request ID that is not one":   aTake("request-1", boardTestLeaseID, "human", "taking the board", 30),
		"a lease ID that is not one":     aTake(boardTestRequestID, "lease-1", "human", "taking the board", 30),
		"one ID doing both jobs":         aTake(boardTestLeaseID, boardTestLeaseID, "human", "taking the board", 30),
		"a class nobody declared":        aTake(boardTestRequestID, boardTestLeaseID, "operator", "taking the board", 30),
		"a class that is shouted":        aTake(boardTestRequestID, boardTestLeaseID, "HUMAN", "taking the board", 30),
		"no class at all":                aTake(boardTestRequestID, boardTestLeaseID, "", "taking the board", 30),
		"no duration":                    aTake(boardTestRequestID, boardTestLeaseID, "human", "taking the board", 0),
		"a duration that runs backwards": aTake(boardTestRequestID, boardTestLeaseID, "human", "taking the board", -30),
		"a duration past eight hours":    aTake(boardTestRequestID, boardTestLeaseID, "human", "taking the board", eightHours+1),
		"no reason":                      aTake(boardTestRequestID, boardTestLeaseID, "human", "", 30),
		"a reason past five hundred":     aTake(boardTestRequestID, boardTestLeaseID, "human", strings.Repeat("x", 501), 30),
		"a reason padded with space":     aTake(boardTestRequestID, boardTestLeaseID, "human", " taking the board", 30),
		"a reason trailing space":        aTake(boardTestRequestID, boardTestLeaseID, "human", "taking the board ", 30),
	} {
		f := &fakeBoardStore{}
		result := tookTheBoard(t, f, body)
		if result.status != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", name, result.status)
		}
		if result.body["detail"] != "invalid board take request" {
			t.Fatalf("%s answered %+v", name, result.body)
		}
		if f.applies != 0 {
			t.Fatalf("%s became a queued waiter", name)
		}
	}
}

// The bounds are inclusive at both ends, which is the pair of the refusals
// above: the door draws its lines exactly rather than somewhere near them.
func TestTheTakeDoorAcceptsItsBoundsExactly(t *testing.T) {
	const eightHours = int64(8 * 60 * 60)

	for name, asked := range map[string]struct {
		why     string
		seconds int64
		want    time.Duration
	}{
		"a lease exactly eight hours long": {why: "long bring-up", seconds: eightHours, want: 8 * time.Hour},
		"a lease of one second":            {why: "smoke", seconds: 1, want: time.Second},
		"a reason of exactly five hundred": {why: strings.Repeat("x", 500), seconds: 30, want: 30 * time.Second},
	} {
		f := &fakeBoardStore{}
		result := tookTheBoard(t, f, aTake(boardTestRequestID, boardTestLeaseID, "ci", asked.why, asked.seconds))
		if result.status != http.StatusOK {
			t.Fatalf("%s answered %d %+v", name, result.status, result.body)
		}
		enqueue, ok := f.command.(board.Enqueue)
		if !ok || enqueue.Waiter.Duration != asked.want {
			t.Fatalf("%s reached the store as %#v", name, f.command)
		}
		if enqueue.Waiter.Reason != asked.why {
			t.Fatalf("%s had its reason rewritten: %q", name, enqueue.Waiter.Reason)
		}
	}
}

// The request cannot name who holds the lease. The holder comes from the
// actor the store authorized, and the fake returns a zero actor, so an
// empty holder here is the door declining to take the caller's word for it
// rather than a field that went missing.
func TestTheTakeDoorBindsTheHolderToTheAuthorizedActor(t *testing.T) {
	f := &fakeBoardStore{}
	result := tookTheBoard(t, f, aTake(boardTestRequestID, boardTestLeaseID, "agent", "running the suite", 600))

	if result.status != http.StatusOK {
		t.Fatalf("status = %d %+v", result.status, result.body)
	}
	enqueue, ok := f.command.(board.Enqueue)
	if !ok {
		t.Fatalf("reached the store as %#v", f.command)
	}
	if enqueue.Waiter.ID != boardTestRequestID || enqueue.Waiter.LeaseID != boardTestLeaseID {
		t.Fatalf("the waiter was rewritten: %#v", enqueue.Waiter)
	}
	if enqueue.Waiter.Holder != "" {
		t.Fatalf("the holder was taken from the request: %q", enqueue.Waiter.Holder)
	}
	if f.version != 3 {
		t.Fatalf("the expected version was not carried: %d", f.version)
	}
}

// The cancel door judges the waiter in its path before anything is applied.
func TestTheCancelDoorJudgesTheWaiterInItsPath(t *testing.T) {
	for name, waiterID := range map[string]string{
		"a waiter that is not an ID": "waiter-1",
		"a UUID that is not an ID":   "f47ac10b-58cc-4372-a567-0e02b2c3d479",
	} {
		f := &fakeBoardStore{}
		response := httptest.NewRecorder()
		boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(response,
			boardTestRequest("POST", "/v1/boards/ek-ra8d2/waiters/"+waiterID+"/cancel", `{"expected_version":1}`))

		if response.Code != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400: %s", name, response.Code, response.Body.String())
		}
		if f.applies != 0 {
			t.Fatalf("%s was applied", name)
		}
	}
}
