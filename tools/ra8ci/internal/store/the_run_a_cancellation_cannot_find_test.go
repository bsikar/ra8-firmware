//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// A cancellation request for a run nobody has.
//
// The identity checks pass, the transaction opens, and the run lock is where
// the plane discovers there is no such run.

func TestIntegrationACancellationRequestForARunThatDoesNotExistIsNotReportedAsCancelled(t *testing.T) {
	st, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	run, err := st.RequestRunCancellation(ctx, mustID(t), "integration-operator")
	// A missing run is reported as missing rather than as a plane that is
	// unwell: the two reach an operator as different answers.
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("an absent run answered %v, want not found", err)
	}
	// The one thing that must not happen is a cancellation reported as
	// taken: an operator told a run was stopped stops looking at it.
	if run.ID != "" || run.CancelRequestedAt != nil {
		t.Fatalf("an absent run came back part-filled: %+v", run)
	}

	// The actor bound is exact. An actor of precisely 256 characters is
	// recordable, so it has to get past the identity check and fail on the
	// missing run instead, which an off-by-one limit would not do.
	if _, err := st.RequestRunCancellation(ctx, mustID(t), strings.Repeat("a", 256)); !errors.Is(err, ErrNotFound) {
		t.Fatalf("an actor at the bound answered %v, want not found", err)
	}
}
