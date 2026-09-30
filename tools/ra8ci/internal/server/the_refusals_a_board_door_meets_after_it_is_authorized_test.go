// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The companion files to this one pin what a board door refuses BEFORE it acts:
// an unauthorized caller, an unreadable body, a store that cannot do the durable
// thing. This file takes the other side. Every case here is a caller the board
// has already authorized and a request it has already read, failing at something
// further in: a store that refuses the read, a snapshot the liveness rule cannot
// judge, a dependency the deployment left unconfigured.
//
// The distinction each case has to carry is WHAT the refusal says about state.
// A denial costs nothing, because nothing happened. A refusal after the board has
// been read, or after a command has been applied, is a different report: the
// side effect landed and only the answer failed, and an operator reading a 4xx
// needs to know which of those two it is holding. So each assertion below pins
// the store counters alongside the status, because the counters are the only
// evidence of how far the request actually got.

// TestBoardRegistrationRefusesMoreThanOnePolicy pins the variadic argument as a
// way to make the policy optional, not a way to pass several and have the board
// pick. A deployment handing two policies has a bug in how it is assembled, and
// the wiring is the only place that can still be caught cheaply.
func TestBoardRegistrationRefusesMoreThanOnePolicy(t *testing.T) {
	err := RegisterBoardRoutes(http.NewServeMux(), &fakeBoardStore{}, nil, "bsikar/ra8-firmware",
		BoardPolicy{}, BoardPolicy{})
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("two policies were accepted: %v", err)
	}
	// One is the whole point of the variadic, so it must still be taken.
	if err := RegisterBoardRoutes(http.NewServeMux(), &fakeBoardStore{}, nil, "bsikar/ra8-firmware",
		BoardPolicy{}); err != nil {
		t.Fatalf("a single policy was refused: %v", err)
	}
}

// TestBoardDoorsRefuseABoardIDThatIsNotOne pins the identifier check ahead of
// the peer lookup. A board ID is interpolated into the audit record and handed
// to the store, so a name carrying anything outside the allowed alphabet is
// refused before either of those sees it, and the refusal is a plain 400 rather
// than the 404 a real denial earns: nothing was looked up, so there is nothing
// to keep quiet about.
func TestBoardDoorsRefuseABoardIDThatIsNotOne(t *testing.T) {
	for _, id := range []string{"ek~ra8d2", "ek:ra8d2", "ek,ra8d2", "ek$ra8d2", "ek(ra8d2)"} {
		t.Run(id, func(t *testing.T) {
			f := &fakeBoardStore{board: heldBoard(time.Now().UTC())}
			w := httptest.NewRecorder()
			boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/"+id, ""))
			if w.Code != http.StatusBadRequest {
				t.Fatalf("status %d, want 400", w.Code)
			}
			if f.peer != nil || f.audits != 0 || f.reads != 0 {
				t.Fatalf("a malformed board ID reached the store: peer=%v audits=%d reads=%d", f.peer != nil, f.audits, f.reads)
			}
		})
	}
	// The allowed alphabet still has to admit an ordinary name, or the check
	// above would be indistinguishable from refusing everything.
	f := &fakeBoardStore{board: heldBoard(time.Now().UTC())}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2_rev.3", ""))
	if w.Code != http.StatusOK || f.reads != 1 {
		t.Fatalf("an ordinary board name was refused: status=%d reads=%d", w.Code, f.reads)
	}
}

// TestBoardReadsCarryTheRefusalTheStoreMet pins the two doors that read the
// board and nothing else. Both hand the store's refusal back rather than
// answering with the zero snapshot, and both record the read, which is what
// separates this 404 from the identical-looking one an unauthorized caller gets.
func TestBoardReadsCarryTheRefusalTheStoreMet(t *testing.T) {
	for _, path := range []string{"/v1/boards/ek-ra8d2", "/v1/boards/ek-ra8d2/liveness"} {
		t.Run(path, func(t *testing.T) {
			f := &fakeBoardStore{getBoardErr: store.ErrNotFound, board: heldBoard(time.Now().UTC())}
			w := httptest.NewRecorder()
			boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", path, ""))
			if w.Code != http.StatusNotFound {
				t.Fatalf("status %d, want 404", w.Code)
			}
			if f.reads != 1 || f.audits != 0 || f.applies != 0 {
				t.Fatalf("read refusal took the wrong path: reads=%d audits=%d applies=%d", f.reads, f.audits, f.applies)
			}
		})
	}
}

// TestLivenessRefusesASnapshotItCannotJudge pins the second failure the liveness
// door can meet: the read succeeded and the board it returned is not one the
// state machine will reason about. Answering "not held" over an invalid snapshot
// would be a liveness report about a board nobody can describe, so it is refused
// as the invalid argument it is.
func TestLivenessRefusesASnapshotItCannotJudge(t *testing.T) {
	f := &fakeBoardStore{} // a zero snapshot: no board ID, so nothing to judge
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("GET", "/v1/boards/ek-ra8d2/liveness", ""))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d, want 400", w.Code)
	}
	if f.reads != 1 {
		t.Fatalf("reads=%d, want the read that produced the unjudgeable snapshot", f.reads)
	}
}

// TestHeartbeatKeepsTheBeatItAlreadyRecorded is the case this file exists for.
// The beat is committed by the time the liveness report is computed, so a
// snapshot the report cannot describe must not read as a beat that failed: the
// refusal is about the answer, and the apply stays counted. A holder reading
// this 400 has already been heard.
func TestHeartbeatKeepsTheBeatItAlreadyRecorded(t *testing.T) {
	f := &fakeBoardStore{} // applies cleanly, then hands back a snapshot with no board ID
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w,
		boardTestRequest("POST", heartbeatPath(), `{"expected_version":41,"generation":7}`))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d, want 400", w.Code)
	}
	if f.applies != 1 {
		t.Fatalf("applies=%d, want the beat that was already recorded", f.applies)
	}
	if f.command == nil {
		t.Fatal("the beat was refused without a command reaching the store")
	}
	if _, ok := f.command.(board.HolderHeartbeat); !ok {
		t.Fatalf("command %T, want a holder heartbeat", f.command)
	}
}

// TestNeutralChallengeCarriesTheStoreRefusalItMet pins the challenge door past
// its verifier and purpose checks. The challenge is what a release or recovery
// is later proven against, so a store that cannot issue one is reported rather
// than answered with an empty proof a caller would then try to use.
func TestNeutralChallengeCarriesTheStoreRefusalItMet(t *testing.T) {
	for _, purpose := range []string{"release", "recovery"} {
		t.Run(purpose, func(t *testing.T) {
			f := &fakeBoardStore{challengeErr: store.ErrNotFound}
			w := httptest.NewRecorder()
			boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w,
				boardTestRequest("POST", "/v1/boards/ek-ra8d2/neutral-challenge",
					`{"expected_version":41,"purpose":"`+purpose+`"}`))
			if w.Code != http.StatusNotFound {
				t.Fatalf("status %d, want 404", w.Code)
			}
			if f.challenges != 1 || f.applies != 0 {
				t.Fatalf("challenge refusal took the wrong path: challenges=%d applies=%d", f.challenges, f.applies)
			}
			if f.version != 41 {
				t.Fatalf("version %d, want the caller's stated 41", f.version)
			}
		})
	}
}

// TestYieldWithoutABudgetRefusesAfterItReadsTheWaiter pins the order of the
// yield door's first two checks. The waiter named in the request is judged
// first, because a malformed one is the caller's error and stays a 400 whatever
// the deployment is missing; only then does an unconfigured handoff budget close
// the door with a retryable 503. Neither reaches the board, so a yield refused
// here has planned nothing and committed nothing.
func TestYieldWithoutABudgetRefusesAfterItReadsTheWaiter(t *testing.T) {
	f := &fakeBoardStore{board: heldBoard(time.Now().UTC())}
	w := httptest.NewRecorder()
	boardTestMux(t, f, nil).ServeHTTP(w,
		boardTestRequest("POST", "/v1/boards/ek-ra8d2/yield",
			`{"expected_version":41,"waiter_id":"`+boardTestRequestID+`"}`))
	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("status %d, want 503", w.Code)
	}
	if f.reads != 0 || f.applies != 0 {
		t.Fatalf("an unconfigured yield touched the board: reads=%d applies=%d", f.reads, f.applies)
	}

	// The waiter check runs first, so a malformed waiter is a 400 even with the
	// same missing budget behind it.
	bad := &fakeBoardStore{board: heldBoard(time.Now().UTC())}
	w = httptest.NewRecorder()
	boardTestMux(t, bad, nil).ServeHTTP(w,
		boardTestRequest("POST", "/v1/boards/ek-ra8d2/yield", `{"expected_version":41,"waiter_id":"not-a-waiter"}`))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d, want 400 for a malformed waiter ahead of the missing budget", w.Code)
	}
	if bad.reads != 0 || bad.applies != 0 {
		t.Fatalf("a malformed waiter touched the board: reads=%d applies=%d", bad.reads, bad.applies)
	}
}
