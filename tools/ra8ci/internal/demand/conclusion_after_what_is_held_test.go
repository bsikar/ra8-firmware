// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"strings"
	"testing"
	"time"
)

// runningDemand is demand the plane has already seen start: the shape that
// carries a forge-clock start stamp into the reconciliation pass.
func runningDemand(jobID int64, startedAt, observedAt time.Time) Event {
	event := queuedDemand(jobID, 1, observedAt)
	event.Phase = PhaseInProgress
	event.StartedAt = startedAt
	event.RunnerName = "ra8-runner-1"
	return event
}

// An ordinary conclusion is stamped with the moment the plane looked, and
// nothing carries it anywhere else.
func TestAnOrdinaryConclusionKeepsThePassesOwnStamp(t *testing.T) {
	held := runningDemand(41, reconcileBase.Add(time.Minute), reconcileBase.Add(2*time.Minute))
	at := reconcileBase.Add(3 * time.Hour)
	if stamp := conclusionStamp(held, at); !stamp.Equal(at) {
		t.Fatalf("an ordinary conclusion was moved: %s", stamp)
	}
}

// The queue-time floor this rule inherited still holds.
func TestAConclusionBeforeTheQueueTimeIsCarriedToIt(t *testing.T) {
	held := queuedDemand(42, 1, reconcileBase)
	at := reconcileBase.Add(-time.Hour)
	if stamp := conclusionStamp(held, at); !stamp.Equal(held.QueuedAt) {
		t.Fatalf("a conclusion before the queue time was not carried to it: %s", stamp)
	}
}

// The start is the stamp that can sit in this host's future, because it came
// from the forge's clock and the pass's own stamp did not.
func TestAConclusionBeforeAHeldStartIsCarriedToTheStart(t *testing.T) {
	started := reconcileBase.Add(90 * time.Second)
	held := runningDemand(43, started, reconcileBase.Add(90*time.Second))
	at := reconcileBase.Add(30 * time.Second)
	if stamp := conclusionStamp(held, at); !stamp.Equal(started) {
		t.Fatalf("a conclusion before the held start was not carried to it: %s", stamp)
	}
}

// The stamp is the latest of the three, whichever one that is.
func TestTheStampIsTheLatestOfWhatThePlaneKnows(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(44, started, started)
	for _, testCase := range []struct {
		name string
		at   time.Time
		want time.Time
	}{
		{"pass is latest", started.Add(time.Hour), started.Add(time.Hour)},
		{"start is latest", started.Add(-time.Second), started},
		{"queue is latest", reconcileBase.Add(-time.Hour), started},
		{"equal to the start", started, started},
		{"one nanosecond past the start", started.Add(1), started.Add(1)},
		{"one nanosecond before the start", started.Add(-1), started},
	} {
		if stamp := conclusionStamp(held, testCase.at); !stamp.Equal(testCase.want) {
			t.Fatalf("%s: stamped %s, wanted %s", testCase.name, stamp, testCase.want)
		}
	}
}

// Demand with no start of its own still falls back to the queue time alone.
func TestDemandWithNoStartStillFallsBackToTheQueueTime(t *testing.T) {
	held := queuedDemand(45, 1, reconcileBase)
	for _, at := range []time.Time{reconcileBase.Add(-time.Hour), reconcileBase, reconcileBase.Add(time.Hour)} {
		stamp := conclusionStamp(held, at)
		if stamp.Before(held.QueuedAt) {
			t.Fatalf("a conclusion was stamped before the queue time: %s", stamp)
		}
		if at.After(held.QueuedAt) && !stamp.Equal(at) {
			t.Fatalf("a conclusion was moved with no start to move it: %s", stamp)
		}
	}
}

// The event the rule exists to make writable is exactly the one Validate
// refuses without it, so the cost is pinned rather than asserted.
func TestTheEventWithoutThisRuleIsRefusedByValidate(t *testing.T) {
	held := runningDemand(46, reconcileBase.Add(90*time.Second), reconcileBase.Add(90*time.Second))
	unclamped := held
	unclamped.Phase = PhaseCompleted
	unclamped.Conclusion = "stale"
	unclamped.DeliveryID = reconcileDeliveryID(held.Key(), PhaseCompleted)
	unclamped.CompletedAt = reconcileBase.Add(30 * time.Second)
	unclamped.ObservedAt = reconcileBase.Add(30 * time.Second)
	err := unclamped.Validate()
	if err == nil || !strings.Contains(err.Error(), "completed before started") {
		t.Fatalf("the refusal this rule avoids is not the one assumed: %v", err)
	}
	if concluded, err := held.concluded(ReconcileAdapter, reconcileBase.Add(30*time.Second)); err != nil {
		t.Fatalf("the same demand could not be concluded: %v", err)
	} else if concluded.CompletedAt.Before(concluded.StartedAt) {
		t.Fatalf("the conclusion still precedes the start: %+v", concluded)
	}
}

// The whole point is the pass: demand the forge forgot is concluded and
// stops being open, instead of failing identically on every pass forever.
func TestThePassConcludesDemandWhoseStartIsAheadOfThisHostsClock(t *testing.T) {
	at := reconcileBase.Add(2 * time.Hour)
	held := runningDemand(47, at.Add(5*time.Minute), reconcileBase.Add(time.Minute))
	store := newMemoryDemand(held)
	reconciler := reconcilerFor(t, &fakeJobs{jobs: map[int64]JobSnapshot{}}, store, at)
	report, err := reconciler.Pass(context.Background())
	if err != nil {
		t.Fatalf("the pass failed on a start ahead of the host clock: %v", err)
	}
	if report.Concluded != 1 || report.Failed != 0 {
		t.Fatalf("demand was not concluded: %+v", report)
	}
	if len(store.recorded) != 1 {
		t.Fatalf("nothing was recorded: %+v", store.recorded)
	}
	written := store.recorded[0]
	if written.Phase != PhaseCompleted || written.Conclusion != "stale" {
		t.Fatalf("the conclusion is not a stale completion: %+v", written)
	}
	if written.CompletedAt.Before(written.StartedAt) || !written.CompletedAt.Equal(held.StartedAt) {
		t.Fatalf("the completion was not carried to the held start: %+v", written)
	}
	if second, err := reconciler.Pass(context.Background()); err != nil || second.Scanned != 0 {
		t.Fatalf("the concluded demand was still open: %+v %v", second, err)
	}
}

// The plane's own observation is when it looked, which is true whatever the
// forge's clock says, so it is not carried with the completion.
func TestTheObservationStampIsNotCarried(t *testing.T) {
	at := reconcileBase.Add(30 * time.Second)
	held := runningDemand(48, reconcileBase.Add(90*time.Second), reconcileBase.Add(90*time.Second))
	concluded, err := held.concluded(ReconcileAdapter, at)
	if err != nil {
		t.Fatal(err)
	}
	if !concluded.ObservedAt.Equal(at) {
		t.Fatalf("the observation stamp was moved: %s", concluded.ObservedAt)
	}
	if !concluded.CompletedAt.Equal(held.StartedAt) {
		t.Fatalf("the completion stamp was not carried: %s", concluded.CompletedAt)
	}
}

// Concluding the same demand twice writes the same evidence, so a repeated
// pass over a row whose start is ahead of the clock is still idempotent.
func TestConcludingTwiceWritesTheSameEvidence(t *testing.T) {
	held := runningDemand(49, reconcileBase.Add(90*time.Second), reconcileBase.Add(90*time.Second))
	first, err := held.concluded(ReconcileAdapter, reconcileBase.Add(30*time.Second))
	if err != nil {
		t.Fatal(err)
	}
	second, err := held.concluded(ReconcileAdapter, reconcileBase.Add(45*time.Second))
	if err != nil {
		t.Fatal(err)
	}
	if first.DeliveryID != second.DeliveryID || !first.CompletedAt.Equal(second.CompletedAt) {
		t.Fatalf("two conclusions of one unit disagree: %+v %+v", first, second)
	}
}

// Every conclusion this rule produces is an event the plane can record,
// across the skews a lagging host produces.
func TestEveryConclusionThisRuleProducesValidates(t *testing.T) {
	for _, skew := range []time.Duration{-time.Hour, -time.Minute, -time.Second, 0, time.Second, time.Hour} {
		for _, held := range []Event{
			queuedDemand(50, 1, reconcileBase),
			runningDemand(51, reconcileBase.Add(2*time.Minute), reconcileBase.Add(2*time.Minute)),
			runningDemand(52, reconcileBase.Add(6*time.Hour), reconcileBase.Add(time.Minute)),
		} {
			concluded, err := held.concluded(ReconcileAdapter, reconcileBase.Add(skew))
			if err != nil {
				t.Fatalf("skew %s, job %d: %v", skew, held.JobID, err)
			}
			if err := concluded.Validate(); err != nil {
				t.Fatalf("skew %s, job %d: recorded an invalid event: %v", skew, held.JobID, err)
			}
		}
	}
}
