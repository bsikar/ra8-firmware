// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Ending a recovery carries a neutral receipt the same way releasing a lease
// does. The route binds the typed command and hands the submission through
// untouched; the server binds it to the board and the approved profile.
func TestRecoveryCompleteCarriesTheNeutralSubmission(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	body := `{"expected_version":5,"challenge_id":"` + boardTestProofID + `","receipt":"c2lnbmF0dXJl"}`
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/recovery/complete", body))

	if w.Code != http.StatusOK || f.applies != 1 || f.version != 5 {
		t.Fatalf("recovery completion did not reach the store: status=%d applies=%d version=%d", w.Code, f.applies, f.version)
	}
	if _, ok := f.command.(board.CompleteRecovery); !ok {
		t.Fatalf("command bound: got %#v, want board.CompleteRecovery", f.command)
	}
	if f.neutral == nil || f.neutral.ChallengeID != boardTestProofID || string(f.neutral.Receipt) != "signature" {
		t.Fatalf("neutral submission: %#v", f.neutral)
	}
}

// The request body may not name the actor, the receipt, or the agent
// high-water: the store binds all three from the authenticated peer and the
// recorded board, so an operator cannot assert a generation it did not observe.
func TestRecoveryCompleteBodyCannotNameTheActorOrHighWater(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	body := `{"expected_version":5,"challenge_id":"` + boardTestProofID + `","receipt":"c2lnbmF0dXJl","actor":"someone-else","agent_high_water":99}`
	boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/recovery/complete", body))

	if w.Code != http.StatusBadRequest || f.applies != 0 {
		t.Fatalf("an unknown field was accepted: status=%d applies=%d", w.Code, f.applies)
	}
}

func TestRecoveryCompleteRefusesAnUnusableSubmission(t *testing.T) {
	for name, body := range map[string]string{
		"no challenge":      `{"expected_version":1,"receipt":"c2ln"}`,
		"invalid challenge": `{"expected_version":1,"challenge_id":"bad","receipt":"c2ln"}`,
		"no receipt":        `{"expected_version":1,"challenge_id":"` + boardTestProofID + `"}`,
	} {
		f := &fakeBoardStore{}
		w := httptest.NewRecorder()
		boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/recovery/complete", body))
		if w.Code != http.StatusBadRequest || f.applies != 0 {
			t.Errorf("%s: status=%d applies=%d", name, w.Code, f.applies)
		}
	}
}

// With no verifier there is nothing that can check a receipt, so the door is
// closed rather than left to accept an unverifiable one.
func TestRecoveryCompleteIsClosedWithoutAVerifier(t *testing.T) {
	f := &fakeBoardStore{}
	w := httptest.NewRecorder()
	body := `{"expected_version":5,"challenge_id":"` + boardTestProofID + `","receipt":"c2ln"}`
	boardTestMux(t, f, nil).ServeHTTP(w, boardTestRequest("POST", "/v1/boards/ek-ra8d2/recovery/complete", body))

	if w.Code != http.StatusServiceUnavailable || f.applies != 0 {
		t.Fatalf("status=%d applies=%d, want 503 and no apply", w.Code, f.applies)
	}
}
