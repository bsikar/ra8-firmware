// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// Everything post refuses before the plane is asked anything, plus what the
// forgiveness walk does with an error carrying nothing. The status is the part
// that matters to a caller: 0 is reserved for a call the plane never answered,
// and only that is worth offering again, so a refusal raised on this side has
// to carry 0 rather than borrow a status nobody sent.

// emptyJoin is a joined error with no members. errors.Join cannot build one,
// but an error type outside this package can, and the forgiveness walk unwraps
// by interface rather than by concrete type.
type emptyJoin struct{}

func (emptyJoin) Error() string   { return "joined nothing" }
func (emptyJoin) Unwrap() []error { return nil }

func TestAnEmptyJoinIsNotForgiven(t *testing.T) {
	// A join with nothing in it vacuously satisfies "every leaf is a cancel",
	// which would let an attempt carrying no evidence at all be filed as a
	// clean cancellation. It has to fail closed instead.
	if everyLeafIs(emptyJoin{}, context.Canceled) {
		t.Fatal("an empty join was forgiven as a cancellation")
	}
	if everyLeafIs(emptyJoin{}, context.DeadlineExceeded) {
		t.Fatal("an empty join was forgiven as a deadline")
	}
	// And the same error nested one level down, since the walk recurses.
	if everyLeafIs(errors.Join(context.Canceled, emptyJoin{}), context.Canceled) {
		t.Fatal("an empty join nested under a cancel was forgiven")
	}
}

func TestABudgetRefusesAGrantThatDoesNotValidate(t *testing.T) {
	unsafe := testAssignment()
	unsafe.AttemptID = ""
	budget, err := assignmentBudget(unsafe, 900)
	if budget != 0 || !errors.Is(err, ErrUnsafeAssignment) {
		t.Fatalf("budget from an invalid grant = %v, %v", budget, err)
	}
	// The grant's own complaint travels with the refusal as text. It is
	// flattened rather than wrapped, so errors.Is cannot reach it and the
	// message is the only place an operator can read which of the two
	// unsafe-grant refusals they are looking at.
	if !strings.Contains(err.Error(), protocol.ErrInvalid.Error()) {
		t.Fatalf("refusal dropped the grant's own complaint: %v", err)
	}
	if errors.Is(err, protocol.ErrInvalid) {
		t.Fatal("the complaint is wrapped after all; assert on the chain instead of the text")
	}
}

func TestPostRefusesARequestItCannotEncodeWithoutAsking(t *testing.T) {
	agent, claims := countingPlane(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	var assignment protocol.Assignment
	status, err := agent.post(context.Background(), "/v1/agents/me/claim", make(chan int), &assignment, true)
	if err == nil {
		t.Fatal("an unencodable request was sent")
	}
	if status != 0 {
		t.Fatalf("status = %d, want 0: the plane was never asked", status)
	}
	if got := claims.Load(); got != 0 {
		t.Fatalf("an unencodable request spent %d calls", got)
	}
}

func TestPostRefusesAnOriginItCannotAddressWithoutAsking(t *testing.T) {
	agent, claims := countingPlane(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	agent.base = "http://\x7f"
	var assignment protocol.Assignment
	status, err := agent.post(context.Background(), "/v1/agents/me/claim", protocol.ClaimRequest{}, &assignment, true)
	if err == nil {
		t.Fatal("an unaddressable origin was called")
	}
	if status != 0 {
		t.Fatalf("status = %d, want 0: the plane was never asked", status)
	}
	if got := claims.Load(); got != 0 {
		t.Fatalf("an unaddressable origin spent %d calls", got)
	}
}
