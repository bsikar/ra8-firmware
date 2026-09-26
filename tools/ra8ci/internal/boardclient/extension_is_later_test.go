// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func leasedUntil(t *testing.T, expiry time.Time) board.Snapshot {
	t.Helper()
	state := activeBoard(t)
	state.Lease.ExpiresAt = expiry
	return state
}

func TestOnlyAStrictlyLaterExpiryExtends(t *testing.T) {
	expiry := time.Date(2026, 9, 26, 18, 30, 0, 0, time.UTC)
	snapshot := leasedUntil(t, expiry)
	for _, row := range []struct {
		name   string
		asked  time.Time
		extend bool
	}{
		{"an hour later extends", expiry.Add(time.Hour), true},
		{"a minute later extends", expiry.Add(time.Minute), true},
		{"a nanosecond later extends", expiry.Add(1), true},
		{"the same instant does not", expiry, false},
		{"a nanosecond earlier does not", expiry.Add(-1), false},
		{"a minute earlier does not", expiry.Add(-time.Minute), false},
		{"an already passed time does not", expiry.Add(-time.Hour), false},
		{"the zero time does not", time.Time{}, false},
	} {
		t.Run(row.name, func(t *testing.T) {
			if got := extensionIsLater(snapshot, row.asked); got != row.extend {
				t.Fatalf("extensionIsLater(%v) = %v, want %v", row.asked, got, row.extend)
			}
		})
	}
}

// The reducer is the authority on what an extension is. This transcribes its
// rule and sweeps both over the same instants, so this client refusing a
// request the server would accept, or sending one the server must refuse,
// fails here.
func TestTheClientAgreesWithTheReducerAboutWhatExtends(t *testing.T) {
	expiry := time.Date(2026, 9, 26, 18, 30, 0, 0, time.UTC)
	snapshot := leasedUntil(t, expiry)
	// board.extend: "c.NewExpiry.IsZero() || !c.NewExpiry.After(s.Lease.ExpiresAt)"
	// is refused as "extension needs reason and later expiry".
	reducerAccepts := func(asked time.Time) bool {
		return !asked.IsZero() && asked.After(snapshot.Lease.ExpiresAt)
	}
	for _, offset := range []time.Duration{
		-8 * time.Hour, -time.Hour, -time.Minute, -time.Second, -1, 0, 1,
		time.Second, time.Minute, time.Hour, 8 * time.Hour,
	} {
		asked := expiry.Add(offset)
		if got, want := extensionIsLater(snapshot, asked), reducerAccepts(asked); got != want {
			t.Fatalf("offset %v: this client says %v, the reducer says %v", offset, got, want)
		}
	}
	if extensionIsLater(snapshot, time.Time{}) != reducerAccepts(time.Time{}) {
		t.Fatal("this client and the reducer disagree about the zero time")
	}
}

// A token carries the expiry that was in force when the grant was read, and
// Extend moves the lease's deadline without handing the caller a new token. So
// the second extension of a run holds a stale figure, and comparing against it
// would send a request the server refuses.
func TestTheSnapshotsExpiryDecidesNotTheTokens(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	// One extension already landed: the lease now runs half an hour past
	// what the token records.
	snapshot := leasedUntil(t, token.ExpiresAt.Add(30*time.Minute))

	asked := token.ExpiresAt.Add(10 * time.Minute)
	if !asked.After(token.ExpiresAt) {
		t.Fatal("fixture is wrong: the ask must look later than the token")
	}
	if extensionIsLater(snapshot, asked) {
		t.Fatal("an ask the lease already covers was treated as an extension")
	}
	if !extensionIsLater(snapshot, token.ExpiresAt.Add(45*time.Minute)) {
		t.Fatal("an ask past the lease's real expiry was refused")
	}
}

// Extend reads the lease through leaseStatus, which already refuses a snapshot
// recording no lease. The rule still decides rather than dereferencing a nil,
// because a helper that panics on a shape its current caller happens to filter
// is a trap for the next one.
func TestNoLeaseIsNotAnExtension(t *testing.T) {
	empty, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	if empty.Lease != nil {
		t.Fatal("fixture is wrong: a fresh board holds no lease")
	}
	if extensionIsLater(empty, time.Now().UTC().Add(time.Hour)) {
		t.Fatal("a board with no lease reported an extension")
	}
}

// The rule reads the deadline and nothing else. Phase, holder and generation
// belong to leaseStatus and the reducer, so it must not start answering for
// them.
func TestTheRuleReadsTheDeadlineAndNothingElse(t *testing.T) {
	expiry := time.Date(2026, 9, 26, 18, 30, 0, 0, time.UTC)
	later := expiry.Add(time.Minute)
	for _, phase := range []board.Phase{
		board.Active, board.YieldRequested, board.Draining,
		board.Recovering, board.Quarantined, board.RecoveryRequired,
	} {
		snapshot := leasedUntil(t, expiry)
		snapshot.Phase = phase
		if !extensionIsLater(snapshot, later) {
			t.Fatalf("phase %v changed the deadline answer", phase)
		}
	}
	foreign := leasedUntil(t, expiry)
	foreign.Lease.Generation = 99
	foreign.Lease.Holder = "somebody-else"
	if !extensionIsLater(foreign, later) {
		t.Fatal("lease identity changed the deadline answer")
	}
}

// The whole point is that the request does not leave this process. These drive
// Extend against a server that fails the test if anything reaches it.
func TestExtendRefusesAnExpiryTheLeaseAlreadyCoversWithoutSendingIt(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	var reads int
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			reads++
			jsonResponse(w, http.StatusOK, state)
			return
		}
		t.Errorf("an extension the lease already covers was sent: %s %s", r.Method, r.URL.Path)
		w.WriteHeader(http.StatusBadRequest)
	})
	defer done()

	for _, asked := range []time.Time{
		token.ExpiresAt,
		token.ExpiresAt.Add(-time.Minute),
		token.ExpiresAt.Add(-time.Hour),
	} {
		if _, err := client.Extend(context.Background(), token, asked, "wrap up"); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("Extend(%v) = %v, want ErrInvalidRequest", asked, err)
		}
	}
	if reads == 0 {
		t.Fatal("the lease was never read, so the refusal was not made against it")
	}
}

func TestExtendStillSendsAGenuineExtension(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	asked := token.ExpiresAt.Add(5 * time.Minute)
	extended := state
	sent := false
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		sent = true
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: extended})
	})
	defer done()

	if _, err := client.Extend(context.Background(), token, asked, "wrap up"); err != nil {
		t.Fatalf("a genuine extension was refused: %v", err)
	}
	if !sent {
		t.Fatal("a genuine extension never reached the server")
	}
}
