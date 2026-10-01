// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The last refusals in the board surface that do not need a database: the two
// failures the yield door can meet once it has already planned, the length
// bound on a board name, and the budget provider's own clock when nothing
// supplied one.
//
// Yield is the only door that reads the board outside the transaction, so it
// is also the only one with two distinct places to fail AFTER the caller has
// been judged. Which of the two happened decides whether anything was promised:
// a read that fails has estimated nothing, while a commit that fails has
// already quoted the requester a target. Both are refusals, and an operator
// cannot tell them apart from the status alone, so both are pinned against the
// store counters that distinguish them.

// TestBoardNameIsBoundedBeforeItIsLookedUp pins the length half of the board
// name check, the companion to the alphabet half. A name is interpolated into
// an audit record and handed to the store, so an unbounded one is refused up
// front rather than written somewhere that has to carry it.
func TestBoardNameIsBoundedBeforeItIsLookedUp(t *testing.T) {
	f := &fakeBoardStore{board: heldBoard(time.Now().UTC())}
	w := httptest.NewRecorder()
	// Every character is in the allowed alphabet, so only the bound can
	// refuse this.
	boardTestMux(t, f, nil).ServeHTTP(w,
		boardTestRequest("GET", "/v1/boards/"+strings.Repeat("b", 129), ""))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status %d, want 400 for a 129-character board name", w.Code)
	}
	if f.peer != nil || f.audits != 0 || f.reads != 0 {
		t.Fatalf("an oversized board name reached the store: peer=%v audits=%d reads=%d", f.peer != nil, f.audits, f.reads)
	}

	// The bound itself is inclusive, or the check would be refusing names it
	// was written to admit.
	atBound := &fakeBoardStore{board: heldBoard(time.Now().UTC())}
	w = httptest.NewRecorder()
	boardTestMux(t, atBound, nil).ServeHTTP(w,
		boardTestRequest("GET", "/v1/boards/"+strings.Repeat("b", 128), ""))
	if w.Code != http.StatusOK || atBound.reads != 1 {
		t.Fatalf("a name exactly at the bound was refused: status=%d reads=%d", w.Code, atBound.reads)
	}
}

// TestYieldRefusesWhenTheBoardCannotBeRead pins the first of the two failures
// after the caller has been judged. The budget is configured and the waiter is
// well formed, so nothing about the request is wrong; the board simply cannot
// be read, and a yield that cannot see the board has nothing to estimate over.
// Nothing is planned and nothing is committed.
func TestYieldRefusesWhenTheBoardCannotBeRead(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman), getBoardErr: store.ErrNotFound}
	budget := &fakeYieldBudget{budget: HandoffBudget{
		Cohort: yieldTestCohort(),
		Bounds: board.DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second},
	}}
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	if w.Code != http.StatusNotFound {
		t.Fatalf("status %d, want 404: %s", w.Code, w.Body.String())
	}
	if f.reads != 1 {
		t.Fatalf("reads=%d, want the read that failed", f.reads)
	}
	if budget.calls != 0 {
		t.Fatalf("budget asked %d times over a board nobody could read", budget.calls)
	}
	if f.applies != 0 {
		t.Fatalf("applies=%d, want nothing committed", f.applies)
	}
}

// TestYieldCarriesACommitRefusalAfterItHasAlreadyPlanned is the other side, and
// the reason this pair is worth separating. By the time the commit is
// attempted, the board has been read, the budget has been spent and a target
// has been computed. The refusal still has to reach the caller unchanged rather
// than being reported as a plan that succeeded, because the only record of that
// target is the transition that did not happen.
func TestYieldCarriesACommitRefusalAfterItHasAlreadyPlanned(t *testing.T) {
	f := &fakeBoardStore{board: yieldTestBoard(board.ClassHuman), applyErr: store.ErrNotFound}
	budget := &fakeYieldBudget{budget: HandoffBudget{
		Cohort: yieldTestCohort(),
		Bounds: board.DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second},
	}}
	w := postYield(t, yieldTestMux(t, f, budget), boardTestWaiterID)
	if w.Code != http.StatusNotFound {
		t.Fatalf("status %d, want 404: %s", w.Code, w.Body.String())
	}
	// The whole path ran: the board was read, the budget was asked, and the
	// commit was attempted once.
	if f.reads != 1 || budget.calls != 1 || f.applies != 1 {
		t.Fatalf("the refusal came from the wrong place: reads=%d budget=%d applies=%d", f.reads, budget.calls, f.applies)
	}
	// And it was the yield that was attempted, not some other command.
	if _, ok := f.command.(board.RequestYield); !ok {
		t.Fatalf("command %T, want board.RequestYield", f.command)
	}
}

// TestStoreYieldBudgetFallsBackToTheWallClock pins the budget provider's own
// clock. NewStoreYieldBudget always supplies one, so this is the guard on a
// value built any other way inside the package: it reads the wall clock rather
// than handing the history read a zero time, which would ask for every sample
// ever filed rather than the ones near now.
func TestStoreYieldBudgetFallsBackToTheWallClock(t *testing.T) {
	st := &recordingBudgetStore{cohort: budgetCohort()}
	before := time.Now().UTC()
	budget := &StoreYieldBudget{store: st} // no clock supplied
	if _, err := budget.HandoffBudget(context.Background(), board.Snapshot{BoardID: "ek-ra8d2"}); err != nil {
		t.Fatal(err)
	}
	if st.readCalls != 1 {
		t.Fatalf("readCalls=%d, want one history read", st.readCalls)
	}
	if st.askedAt.IsZero() {
		t.Fatal("the history was read as of the zero time, which asks for every sample ever filed")
	}
	if st.askedAt.Before(before) || st.askedAt.After(time.Now().UTC().Add(time.Minute)) {
		t.Fatalf("history read as of %v, want a time from the wall clock", st.askedAt)
	}
}
