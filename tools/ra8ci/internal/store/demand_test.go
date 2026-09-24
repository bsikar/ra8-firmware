// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

func demandAt(phase demand.Phase, jobID int64, attempt int) demand.Event {
	queued := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	event := demand.Event{Adapter: "github-app", DeliveryID: "d-1", Phase: phase,
		JobID: jobID, RunID: 900, RunAttempt: attempt, Owner: "bsikar", Repository: "ra8-firmware",
		Workflow: "ci", JobName: "build", CommitSHA: "0123456789abcdef0123456789abcdef01234567",
		Labels: []string{"self-hosted", "ra8"}, QueuedAt: queued, ObservedAt: queued.Add(time.Second)}
	if phase != demand.PhaseQueued {
		event.StartedAt = queued.Add(time.Minute)
	}
	if phase == demand.PhaseCompleted {
		event.Conclusion = "success"
		event.CompletedAt = queued.Add(2 * time.Minute)
	}
	return event
}

// The ordering rule is the demand package's, not this one's: what the store
// writes must follow Supersedes exactly, or the same job could end up with
// two different answers depending on which side was asked.
func TestDemandWriteFollowsSupersedes(t *testing.T) {
	phases := []demand.Phase{demand.PhaseQueued, demand.PhaseInProgress, demand.PhaseCompleted}
	for _, held := range phases {
		for _, incoming := range phases {
			want := DemandStale
			if demandAt(incoming, 42, 1).Supersedes(demandAt(held, 42, 1)) {
				want = DemandSuperseded
			}
			got := demandWrite(demandAt(incoming, 42, 1), demandAt(held, 42, 1), true)
			if got != want {
				t.Fatalf("held %s, incoming %s: got %s, want %s", held, incoming, got, want)
			}
		}
	}
}

func TestDemandWriteFirstSightIsAccepted(t *testing.T) {
	for _, phase := range []demand.Phase{demand.PhaseQueued, demand.PhaseInProgress, demand.PhaseCompleted} {
		if got := demandWrite(demandAt(phase, 42, 1), demand.Event{}, false); got != DemandAccepted {
			t.Fatalf("first %s: got %s, want %s", phase, got, DemandAccepted)
		}
	}
}

// A retried run is new demand, not a later phase of the old one, so a
// completed attempt 1 must never stand in the way of a queued attempt 2.
func TestDemandWriteKeepsRunAttemptsApart(t *testing.T) {
	queuedRetry := demandAt(demand.PhaseQueued, 42, 2)
	completedFirst := demandAt(demand.PhaseCompleted, 42, 1)
	if queuedRetry.Key() == completedFirst.Key() {
		t.Fatal("run attempts share a key")
	}
	if got := demandWrite(queuedRetry, completedFirst, true); got != DemandStale {
		t.Fatalf("got %s, want %s: a different key must not move the row", got, DemandStale)
	}
}

// The key the store writes has to satisfy the column's own CHECK, or the
// first delivery from a real job id fails at the database instead of here.
func TestDemandKeyMatchesColumnShape(t *testing.T) {
	for _, event := range []demand.Event{demandAt(demand.PhaseQueued, 1, 1),
		demandAt(demand.PhaseCompleted, 9007199254740993, 1000)} {
		key := event.Key()
		for i, r := range key {
			if (r < '0' || r > '9') && r != '/' {
				t.Fatalf("key %q has %q at %d, outside the stored shape", key, r, i)
			}
		}
	}
}

func TestDemandLabelsRoundTrip(t *testing.T) {
	encoded, err := demandLabels([]string{"self-hosted", "ra8"})
	if err != nil {
		t.Fatal(err)
	}
	var decoded []string
	if err := json.Unmarshal(encoded, &decoded); err != nil {
		t.Fatal(err)
	}
	if len(decoded) != 2 || decoded[0] != "self-hosted" || decoded[1] != "ra8" {
		t.Fatalf("got %v", decoded)
	}
}

func TestDemandNullableColumns(t *testing.T) {
	if nullableText("") != nil || nullableText("x") != any("x") {
		t.Fatal("empty text must store NULL, a value must store itself")
	}
	if nullableTime(time.Time{}) != nil {
		t.Fatal("the zero time must store NULL, not year zero")
	}
	moment := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	if nullableTime(moment) != any(moment) {
		t.Fatal("a real time must store itself")
	}
}
