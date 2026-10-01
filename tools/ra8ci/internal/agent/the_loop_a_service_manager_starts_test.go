// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"sync/atomic"
	"testing"
	"time"
)

// Run is the loop a service manager actually starts, and its whole contract is
// when it comes back. Returning keeps the host out of the way while the plane
// fences whatever it was holding; not returning is a host that keeps claiming
// work it has already proved it cannot carry. So the loop is pinned on both
// sides: which errors end it, and the idle wait it keeps polling across.

func countingPlane(t *testing.T, answer http.HandlerFunc) (*Agent, *atomic.Int64) {
	t.Helper()
	var claims atomic.Int64
	agent, server := testAgent(t, func(w http.ResponseWriter, r *http.Request) {
		claims.Add(1)
		answer(w, r)
	})
	t.Cleanup(server.Close)
	return agent, &claims
}

func TestRunStopsOnAProtocolFailure(t *testing.T) {
	agent, claims := countingPlane(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	err := agent.Run(ctx)
	if !errors.Is(err, ErrServerProtocol) {
		t.Fatalf("run answered %v", err)
	}
	// The deadline is generous on purpose: if the loop had swallowed the
	// refusal and polled on, this would come back as DeadlineExceeded five
	// seconds later instead, and the assertion above would be the only thing
	// telling them apart.
	if ctx.Err() != nil {
		t.Fatalf("run outlived its own error, ctx=%v", ctx.Err())
	}
	if got := claims.Load(); got != 1 {
		t.Fatalf("claims after a refused poll = %d, want 1", got)
	}
}

func TestRunStopsOnAnUnsafeGrantSoThePlaneCanFenceIt(t *testing.T) {
	grant := testAssignment()
	grant.FencingToken = 0
	agent, claims := countingPlane(t, func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if err := json.NewEncoder(w).Encode(grant); err != nil {
			t.Errorf("encode grant: %v", err)
		}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	err := agent.Run(ctx)
	// An attempt the plane has already handed over is the one case where
	// polling on is worse than exiting: the attempt is fenced on the plane
	// and this host is the only one who could report against it, so it has
	// to stop and let the service manager bring it back clean.
	if !errors.Is(err, ErrUnsafeAssignment) {
		t.Fatalf("run answered %v", err)
	}
	if ctx.Err() != nil {
		t.Fatalf("run outlived the unsafe grant, ctx=%v", ctx.Err())
	}
	if got := claims.Load(); got != 1 {
		t.Fatalf("claims after an unsafe grant = %d, want 1", got)
	}
}

func TestRunKeepsPollingAcrossTheIdleWait(t *testing.T) {
	agent, claims := countingPlane(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	// Long enough for the 250ms idle wait to elapse twice. A second claim can
	// only arrive on the far side of that wait, so it is the evidence that an
	// empty answer pauses the loop rather than ending it or spinning it.
	ctx, cancel := context.WithTimeout(context.Background(), 700*time.Millisecond)
	defer cancel()
	if err := agent.Run(ctx); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("run answered %v", err)
	}
	got := claims.Load()
	if got < 2 {
		t.Fatalf("claims across 700ms of empty answers = %d, want at least 2", got)
	}
	// And paced by the wait rather than spinning on the plane: without it,
	// a 1ms poll wait would put hundreds of claims through in this window.
	if got > 10 {
		t.Fatalf("claims across 700ms of empty answers = %d, the idle wait is not pacing them", got)
	}
}

func TestRunAsksNothingOfAnAlreadyCancelledContext(t *testing.T) {
	agent, claims := countingPlane(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := agent.Run(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("run answered %v", err)
	}
	if got := claims.Load(); got != 0 {
		t.Fatalf("a cancelled agent spent %d claims", got)
	}
}
