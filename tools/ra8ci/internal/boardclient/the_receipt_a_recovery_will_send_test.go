// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The leftovers of the recovery guard: the challenge fields
// TestFinishRecoveryRefusesAChallengeThatDoesNotFit does not spoil, and the
// receipt bounds on the far side of the signature. A receipt is the only
// thing a neutral box says about a board it cannot see, so an unusable one
// is refused here rather than sent for the server to puzzle over.

// recoveryAsked serves a recovering board and one challenge, and reports
// whether a completion was ever submitted.
func recoveryAsked(t *testing.T, state board.Snapshot, challenge store.NeutralChallenge, submitted *int) (*Client, func()) {
	t.Helper()
	return testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/neutral-challenge":
			jsonResponse(w, http.StatusCreated, challenge)
		default:
			*submitted++
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
		}
	})
}

// The identity of the challenge matters as much as its contents: an ID the
// server cannot look up, or a challenge with nothing one-use about it, is
// not a thing to sign.
func TestFinishRecoveryRefusesAChallengeWithNothingToLookUp(t *testing.T) {
	state := recoveringInProgress(t)
	for name, spoil := range map[string]func(store.NeutralChallenge) store.NeutralChallenge{
		"an ID that is not one": func(c store.NeutralChallenge) store.NeutralChallenge { c.ID = "challenge-3"; return c },
		"no ID at all":          func(c store.NeutralChallenge) store.NeutralChallenge { c.ID = ""; return c },
		"no nonce":              func(c store.NeutralChallenge) store.NeutralChallenge { c.Nonce = ""; return c },
		"no fixture revision":   func(c store.NeutralChallenge) store.NeutralChallenge { c.FixtureRevision = ""; return c },
	} {
		producer := &receiptProducer{receipt: []byte("x")}
		submitted := 0
		c, closeServer := recoveryAsked(t, state, spoil(recoveryChallenge(state)), &submitted)
		if _, err := c.FinishRecovery(context.Background(), "ek-ra8d2", producer); !errors.Is(err, ErrInvalidNeutralProof) {
			t.Errorf("%s: got %v, want ErrInvalidNeutralProof", name, err)
		}
		if producer.seen.ID != "" || submitted != 0 {
			t.Errorf("%s: an unusable challenge was signed or submitted", name)
		}
		closeServer()
	}
}

// A neutral box that cannot sign says so, and that answer is carried back
// rather than turned into a completion with nothing behind it.
func TestFinishRecoveryCarriesBackABoxThatWillNotSign(t *testing.T) {
	state := recoveringInProgress(t)
	refused := errors.New("no key material")
	producer := &receiptProducer{err: refused}
	submitted := 0
	c, closeServer := recoveryAsked(t, state, recoveryChallenge(state), &submitted)
	defer closeServer()

	if _, err := c.FinishRecovery(context.Background(), "ek-ra8d2", producer); !errors.Is(err, refused) {
		t.Fatalf("a box that would not sign = %v", err)
	}
	if producer.seen.ID == "" {
		t.Fatal("the box was never asked")
	}
	if submitted != 0 {
		t.Fatal("a recovery was completed without a receipt")
	}
}

// The receipt bounds, exact on both sides. Nothing signed is not a receipt,
// and a receipt past the bound is refused before it is sent rather than
// after the server has read it.
func TestFinishRecoveryHoldsTheReceiptToItsBounds(t *testing.T) {
	state := recoveringInProgress(t)
	for name, receipt := range map[string][]byte{
		"nothing signed":    nil,
		"an empty receipt":  {},
		"one byte too much": make([]byte, 65537),
	} {
		producer := &receiptProducer{receipt: receipt}
		submitted := 0
		c, closeServer := recoveryAsked(t, state, recoveryChallenge(state), &submitted)
		if _, err := c.FinishRecovery(context.Background(), "ek-ra8d2", producer); !errors.Is(err, ErrInvalidNeutralProof) {
			t.Errorf("%s: got %v, want ErrInvalidNeutralProof", name, err)
		}
		if submitted != 0 {
			t.Errorf("%s: an unusable receipt was submitted", name)
		}
		closeServer()
	}

	for name, receipt := range map[string][]byte{
		"a one-byte receipt":   make([]byte, 1),
		"a receipt at the cap": make([]byte, 65536),
	} {
		producer := &receiptProducer{receipt: receipt}
		submitted := 0
		c, closeServer := recoveryAsked(t, state, recoveryChallenge(state), &submitted)
		if _, err := c.FinishRecovery(context.Background(), "ek-ra8d2", producer); err != nil {
			t.Errorf("%s: %v", name, err)
		}
		if submitted != 1 {
			t.Errorf("%s: submitted %d times", name, submitted)
		}
		closeServer()
	}
}

// A server that will not mint a challenge leaves the recovery where it was:
// the refusal is reported, and nothing is completed on a challenge that was
// never issued.
func TestFinishRecoveryReportsAChallengeThatWasNeverIssued(t *testing.T) {
	state := recoveringInProgress(t)
	producer := &receiptProducer{receipt: []byte("x")}
	submitted := 0
	c, closeServer := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/boards/ek-ra8d2":
			jsonResponse(w, http.StatusOK, state)
		case "/v1/boards/ek-ra8d2/neutral-challenge":
			jsonResponse(w, http.StatusServiceUnavailable, map[string]any{"error": "neutral box unreachable"})
		default:
			submitted++
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
		}
	})
	defer closeServer()

	_, err := c.FinishRecovery(context.Background(), "ek-ra8d2", producer)
	if err == nil {
		t.Fatal("a challenge that was never issued was treated as one")
	}
	var status *HTTPError
	if !errors.As(err, &status) || status.Status != http.StatusServiceUnavailable {
		t.Fatalf("got %v, want the server's own refusal", err)
	}
	if producer.seen.ID != "" || submitted != 0 {
		t.Fatal("a challenge that was never issued was signed or submitted")
	}
}
