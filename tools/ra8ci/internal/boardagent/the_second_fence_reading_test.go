// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// steppedClock answers with base until the agent has asked for the time more
// than settled times, and with base plus leap after that. Counting the asks is
// what lets a test land a clock jump on one specific reading rather than on
// every reading in the call.
func steppedClock(base time.Time, settled int, leap time.Duration) func() time.Time {
	asked := 0
	return func() time.Time {
		asked++
		if asked > settled {
			return base.Add(leap)
		}
		return base
	}
}

// clockAsksPerPrecheck measures how many times one settled pre-check reads the
// clock, with the fence already seeded, so a later jump can be aimed past it.
func clockAsksPerPrecheck(t *testing.T, agent *Agent, token boardclient.LeaseToken, base time.Time) int {
	t.Helper()
	agent.clock = func() time.Time { return base }
	if err := agent.CanStartSegment(context.Background(), token, time.Second, 0); err != nil {
		t.Fatalf("the pre-check refused a settled segment: %v", err)
	}
	asked := 0
	agent.clock = func() time.Time { asked++; return base }
	if err := agent.CanStartSegment(context.Background(), token, time.Second, 0); err != nil {
		t.Fatalf("the pre-check refused a settled segment: %v", err)
	}
	return asked
}

// The fence is consulted a second time after the segment has been opened, and
// that reading is the one that matters: the pre-check can pass and the local
// deadline still lapse before the operation would start. When it does, the
// operation never runs and the segment the server already opened is finished
// as failed rather than left dangling.
func TestALeaseThatLapsesAfterTheBeginNeverRunsTheOperation(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	base := time.Now().UTC()
	settled := clockAsksPerPrecheck(t, agent, token, base)
	begins := client.begins
	agent.clock = steppedClock(base, settled+2, 2*time.Minute)
	ran := 0
	segment, err := agent.RunSegment(context.Background(), token, hilAttemptAssignment(token.BoardID).Attempt.ID,
		"flash", 2*time.Second, 0, func(context.Context) error {
			ran++
			return nil
		})
	if err == nil {
		t.Fatal("a lapsed local deadline was not refused")
	}
	var refusal *board.Error
	if !errors.As(err, &refusal) || refusal.Code != board.Expired {
		t.Fatalf("the refusal does not name the lapsed deadline: %v", err)
	}
	if ran != 0 {
		t.Fatalf("the operation ran under a lapsed deadline: %d", ran)
	}
	if client.begins != begins+1 || segment.ID == "" {
		t.Fatalf("the segment was not opened before the refusal: begins=%d segment=%+v", client.begins, segment)
	}
	if len(client.finishes) == 0 || client.finishes[len(client.finishes)-1] != "failed" {
		t.Fatalf("the opened segment was not finished as failed: %v", client.finishes)
	}
}

// A lease extended on the server does not revive a local fence that already
// lapsed. The server snapshot alone would authorize the segment, so this is
// the refusal that keeps the two checks from substituting for each other.
func TestAnExtensionDoesNotReviveALapsedFence(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	base := time.Now().UTC()
	agent.clock = func() time.Time { return base }
	if err := agent.CanStartSegment(context.Background(), token, time.Second, 0); err != nil {
		t.Fatalf("the pre-check refused a settled segment: %v", err)
	}
	extended := *client.state.Lease
	extended.DeadlineVersion++
	extended.ExpiresAt = extended.GrantedAt.Add(30 * time.Minute)
	client.state.Lease = &extended
	agent.clock = func() time.Time { return base.Add(2 * time.Minute) }
	err := agent.CanStartSegment(context.Background(), token, time.Second, 0)
	if err == nil {
		t.Fatal("an extension revived a fence that had already lapsed")
	}
	var refusal *board.Error
	if !errors.As(err, &refusal) || refusal.Code != board.Expired {
		t.Fatalf("the refusal does not name the lapsed deadline: %v", err)
	}
}
