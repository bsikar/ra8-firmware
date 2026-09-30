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

// The board doors registered outside board.go itself (yield, heartbeat,
// liveness, the two segment doors, the two HIL attempt doors and the HIL
// observation history) open the same way as the ones in it and had the same
// gap: their refusals were never driven. This is the companion table to
// the_doors_a_board_refuses_before_it_acts_test.go, which covers board.go's
// own twelve.
//
// Two of these doors reach the body decoder with an ordinary store behind
// them. The other five ask first whether the store can do the durable thing
// at all, which is a type assertion, and say so when it cannot. That refusal
// is worth pinning on its own: a plane whose store cannot keep durable board
// segments must answer that it is not configured, not accept the request and
// drop it.

func boardSatelliteDoors() []boardDoor {
	const board = "/v1/boards/ek-ra8d2"
	const lease = boardTestLeaseID
	const attempt = boardTestProofID
	return []boardDoor{
		{"GET", board + "/liveness", "board.liveness", ""},
		{"POST", board + "/yield", "board.yield", `{"expected_version":1}`},
		{"POST", board + "/leases/" + lease + "/heartbeat", "board.heartbeat", `{"expected_version":1}`},
		{"POST", board + "/segments/begin", "board.segment.begin", `{"expected_version":1}`},
		{"POST", board + "/segments/" + attempt + "/finish", "board.segment.finish", `{"expected_version":1}`},
		{"POST", board + "/hil-attempts/claim", "board.hil.claim", `{"expected_version":1}`},
		{"POST", board + "/hil-attempts/" + attempt + "/complete", "board.hil.complete", `{"expected_version":1}`},
		{"POST", board + "/hil-observations", "board.hil.history", `{"expected_version":1}`},
	}
}

// TestNoSatelliteBoardDoorActsForACallerTheStoreTurnedAway holds the same four
// facts the main table holds: 404 rather than a 403 that confirms the board,
// one audit under this door's own action, and no store questioned. Every one
// of these doors would otherwise go on to a durable write or a snapshot read.
func TestNoSatelliteBoardDoorActsForACallerTheStoreTurnedAway(t *testing.T) {
	for _, door := range boardSatelliteDoors() {
		t.Run(door.action, func(t *testing.T) {
			f := &fakeBoardStore{authorizeErr: store.ErrDenied}
			w := httptest.NewRecorder()
			boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest(door.method, door.path, door.body))

			if w.Code != http.StatusNotFound {
				t.Fatalf("a denied caller was answered %d, want 404", w.Code)
			}
			if f.applies != 0 || f.reads != 0 || f.challenges != 0 {
				t.Fatalf("the store was worked for a denied caller: applies=%d reads=%d challenges=%d",
					f.applies, f.reads, f.challenges)
			}
			if f.audits != 1 || f.action != door.action {
				t.Fatalf("the denial was audited %d times as %q, want once as %q", f.audits, f.action, door.action)
			}
		})
	}
}

// TestADoorSaysSoWhenItsStoreCannotDoTheDurableThing pins the type-assertion
// refusal. These five doors need more of a store than the board interface
// asks for: durable segments, lease-bound HIL completion, lease-aware HIL
// dispatch, a reviewed catalog. A plane wired without them is a plane that
// cannot serve these doors, and it says that with a retryable 503 rather than
// taking the request and losing it.
func TestADoorSaysSoWhenItsStoreCannotDoTheDurableThing(t *testing.T) {
	const board = "/v1/boards/ek-ra8d2"
	for _, door := range []boardDoor{
		{"POST", board + "/segments/begin", "board.segment.begin", `{"expected_version":1}`},
		{"POST", board + "/segments/" + boardTestProofID + "/finish", "board.segment.finish", `{"expected_version":1}`},
		{"POST", board + "/hil-attempts/claim", "board.hil.claim", `{"expected_version":1}`},
		{"POST", board + "/hil-attempts/" + boardTestProofID + "/complete", "board.hil.complete", `{"expected_version":1}`},
		{"POST", board + "/hil-observations", "board.hil.history", `{"expected_version":1}`},
	} {
		t.Run(door.action, func(t *testing.T) {
			f := &fakeBoardStore{}
			w := httptest.NewRecorder()
			boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest(door.method, door.path, door.body))

			if w.Code != http.StatusServiceUnavailable {
				t.Fatalf("a door its store cannot serve answered %d, want 503", w.Code)
			}
			if f.applies != 0 {
				t.Fatalf("%d commands were applied by a door that is not configured", f.applies)
			}
			// The caller was authorized, so nothing about this is a denial.
			if f.audits != 0 {
				t.Fatalf("an unconfigured door audited the caller as denied %d times", f.audits)
			}
		})
	}
}

// TestTheSatelliteDoorsThatDecodeRefuseABodyTheyCannotRead covers the two
// doors that reach the decoder with an ordinary store: the rest are stopped
// by the configuration check above, which is itself the thing being pinned
// there.
func TestTheSatelliteDoorsThatDecodeRefuseABodyTheyCannotRead(t *testing.T) {
	const board = "/v1/boards/ek-ra8d2"
	for _, door := range []boardDoor{
		{"POST", board + "/yield", "board.yield", `{"expected_version":1}`},
		{"POST", board + "/leases/" + boardTestLeaseID + "/heartbeat", "board.heartbeat", `{"expected_version":1}`},
	} {
		t.Run(door.action, func(t *testing.T) {
			f := &fakeBoardStore{}
			w := httptest.NewRecorder()
			r := httptest.NewRequest(door.method, door.path, strings.NewReader(door.body))
			r.Header.Set("Content-Type", "text/plain")
			r.TLS = boardTestRequest("GET", board, "").TLS
			boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, r)
			if w.Code != http.StatusUnsupportedMediaType || f.applies != 0 {
				t.Fatalf("a body the door cannot read was answered %d with %d applies, want 415 and none",
					w.Code, f.applies)
			}

			for _, body := range []string{`{`, `{"no_such_field":1}`, `{"expected_version":1} {"expected_version":2}`} {
				f := &fakeBoardStore{}
				w := httptest.NewRecorder()
				boardTestMux(t, f, fakeNeutralVerifier{}).ServeHTTP(w, boardTestRequest(door.method, door.path, body))
				if w.Code != http.StatusBadRequest || f.applies != 0 {
					t.Fatalf("body %q was answered %d with %d applies, want 400 and none", body, w.Code, f.applies)
				}
			}
		})
	}
}
