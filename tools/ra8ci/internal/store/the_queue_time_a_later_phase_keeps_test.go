//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// The queue time a later phase keeps.
//
// Two deliveries for one unit of demand need not agree on when it was
// queued. The row keeps the earliest queue time either reported, so a
// completion whose own times agree is never refused against a later queue
// time it did not carry.
func TestIntegrationTheQueueTimeALaterPhaseKeeps(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	nextJob := func() int64 { return time.Now().UnixNano()%1_000_000_000_000 + 1 }

	t.Run("a completion that predates the queue time held", func(t *testing.T) {
		job := nextJob()
		queued := demandFixture(t, job, 1, demand.PhaseQueued, "late-queue-"+mustID(t))
		queued.QueuedAt = queued.QueuedAt.Add(time.Hour)
		queued.ObservedAt = queued.QueuedAt
		if _, err := s.RecordDemandEvent(ctx, queued); err != nil {
			t.Fatal(err)
		}
		done := demandFixture(t, job, 1, demand.PhaseCompleted, "early-done-"+mustID(t))
		outcome, err := s.RecordDemandEvent(ctx, done)
		if err != nil || outcome != DemandSuperseded {
			t.Fatalf("the completion was not taken: %q %v", outcome, err)
		}
		held, err := s.GetDemandEvent(ctx, queued.Key())
		if err != nil {
			t.Fatal(err)
		}
		if held.Event.Phase != demand.PhaseCompleted || !held.Event.QueuedAt.Equal(done.QueuedAt) ||
			held.Version != 2 {
			t.Fatalf("held phase=%s queued=%s version=%d, want completed at %s version 2",
				held.Event.Phase, held.Event.QueuedAt, held.Version, done.QueuedAt)
		}
	})

	t.Run("a completion that reports a later queue time", func(t *testing.T) {
		job := nextJob()
		queued := demandFixture(t, job, 1, demand.PhaseQueued, "first-queue-"+mustID(t))
		if _, err := s.RecordDemandEvent(ctx, queued); err != nil {
			t.Fatal(err)
		}
		done := demandFixture(t, job, 1, demand.PhaseCompleted, "later-done-"+mustID(t))
		done.QueuedAt = done.QueuedAt.Add(30 * time.Second)
		done.ObservedAt = done.QueuedAt
		done.StartedAt = done.QueuedAt.Add(time.Minute)
		done.CompletedAt = done.QueuedAt.Add(2 * time.Minute)
		if outcome, err := s.RecordDemandEvent(ctx, done); err != nil || outcome != DemandSuperseded {
			t.Fatalf("the completion was not taken: %q %v", outcome, err)
		}
		held, err := s.GetDemandEvent(ctx, queued.Key())
		if err != nil {
			t.Fatal(err)
		}
		if held.Event.Phase != demand.PhaseCompleted || !held.Event.QueuedAt.Equal(queued.QueuedAt) {
			t.Fatalf("held phase=%s queued=%s, want completed keeping %s",
				held.Event.Phase, held.Event.QueuedAt, queued.QueuedAt)
		}
	})
}
