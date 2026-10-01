// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Every board door opens the same way: it asks whether this certificate may
// do this thing to this board, and it decodes the body only after the answer
// is yes. Both of those refusals had been driven on the status door alone,
// which is the one door that reads rather than writes. The doors that change
// the board, which are the ones a refusal actually protects, went in
// untested: a handler that forgot its `if !ok { return }` would have applied
// a command for a caller the store had just turned away, and nothing in the
// package would have noticed.
//
// The table below is every write door board.go registers. It is written as
// the route plus the action name the denial must be audited under, because
// the audited action is what an operator reading the denial log searches on:
// a door auditing another door's name is a denial recorded against the wrong
// question.

type boardDoor struct {
	method, path, action, body string
}

func boardWriteDoors() []boardDoor {
	const board = "/v1/boards/ek-ra8d2"
	const waiter = boardTestRequestID
	const lease = boardTestLeaseID
	return []boardDoor{
		{"POST", board + "/take", "board.take", `{"expected_version":1}`},
		{"POST", board + "/waiters/" + waiter + "/cancel", "board.cancel", `{"expected_version":1}`},
		{"POST", board + "/checkpoint", "board.checkpoint", `{"expected_version":1}`},
		{"POST", board + "/leases/" + lease + "/free", "board.free", `{"expected_version":1}`},
		{"POST", board + "/leases/" + lease + "/extend", "board.extend", `{"expected_version":1}`},
		{"POST", board + "/neutral-challenge", "board.neutral_challenge", `{"expected_version":1}`},
		{"POST", board + "/agent/ack", "board.agent.ack", `{"expected_version":1}`},
		{"POST", board + "/agent/observe", "board.agent.observe", `{"expected_version":1}`},
		{"POST", board + "/agent/unavailable", "board.agent.unavailable", `{"expected_version":1}`},
		{"POST", board + "/recovery/start", "board.recovery.start", `{"expected_version":1}`},
		{"POST", board + "/recovery/complete", "board.recovery.complete", `{"expected_version":1}`},
		{"POST", board + "/quarantine", "board.quarantine", `{"expected_version":1}`},
	}
}

// TestNoBoardDoorActsForACallerTheStoreTurnedAway drives every write door with
// a store that denies, and holds each to the same four facts: the caller is
// told the board is not there rather than that it exists and they may not
// touch it, the denial is audited under this door's own action, no command is
// applied, and no snapshot is read.
func TestNoBoardDoorActsForACallerTheStoreTurnedAway(t *testing.T) {
	for _, door := range boardWriteDoors() {
		t.Run(door.action, func(t *testing.T) {
			f := &fakeBoardStore{authorizeErr: store.ErrDenied}
			w := httptest.NewRecorder()
			boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest(door.method, door.path, door.body))

			if w.Code != http.StatusNotFound {
				t.Fatalf("a denied caller was answered %d, want 404", w.Code)
			}
			if f.applies != 0 {
				t.Fatalf("%d commands were applied for a denied caller", f.applies)
			}
			if f.reads != 0 || f.challenges != 0 {
				t.Fatalf("the store was questioned for a denied caller: reads=%d challenges=%d", f.reads, f.challenges)
			}
			if f.audits != 1 {
				t.Fatalf("the denial was audited %d times, want once", f.audits)
			}
			if f.action != door.action {
				t.Fatalf("the denial was audited as %q, want %q", f.action, door.action)
			}
			if f.id != "ek-ra8d2" || f.repo != "bsikar/ra8-firmware" {
				t.Fatalf("the denial was recorded against %q in %q", f.id, f.repo)
			}
		})
	}
}

// TestNoBoardDoorActsOnABodyItCannotRead drives every write door with a body
// the decoder will not take, from a caller the store authorizes. The door
// must refuse on the body alone and apply nothing: a command built from a
// half-read request is a command nobody sent.
func TestNoBoardDoorActsOnABodyItCannotRead(t *testing.T) {
	for _, door := range boardWriteDoors() {
		t.Run(door.action, func(t *testing.T) {
			f := &fakeBoardStore{}
			w := httptest.NewRecorder()
			r := httptest.NewRequest(door.method, door.path, strings.NewReader(door.body))
			r.Header.Set("Content-Type", "text/plain")
			r.TLS = boardTestRequest("GET", "/v1/boards/ek-ra8d2", "").TLS
			boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, r)

			if w.Code != http.StatusUnsupportedMediaType {
				t.Fatalf("a body the door cannot read was answered %d, want 415", w.Code)
			}
			if f.applies != 0 || f.challenges != 0 {
				t.Fatalf("the door acted on a body it never read: applies=%d challenges=%d", f.applies, f.challenges)
			}
			if f.audits != 0 {
				t.Fatalf("an authorized caller was audited as denied %d times", f.audits)
			}
		})
	}
}

// TestNoBoardDoorActsOnAnUnreadableJSONBody is the same rule one step in: the
// content type is right and the JSON is not. DisallowUnknownFields is what
// makes a field nobody declared a refusal rather than a silently dropped
// instruction, so a client that misspells a field is told, instead of having
// its command applied with that field's default.
func TestNoBoardDoorActsOnAnUnreadableJSONBody(t *testing.T) {
	for _, door := range boardWriteDoors() {
		t.Run(door.action, func(t *testing.T) {
			for _, body := range []string{`{`, `{"no_such_field":1}`, `{"expected_version":1} {"expected_version":2}`} {
				f := &fakeBoardStore{}
				w := httptest.NewRecorder()
				boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest(door.method, door.path, body))

				if w.Code != http.StatusBadRequest {
					t.Fatalf("body %q was answered %d, want 400", body, w.Code)
				}
				if f.applies != 0 || f.challenges != 0 {
					t.Fatalf("body %q was acted on: applies=%d challenges=%d", body, f.applies, f.challenges)
				}
			}
		})
	}
}
