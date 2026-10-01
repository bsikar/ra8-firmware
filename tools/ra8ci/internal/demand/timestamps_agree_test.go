// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"errors"
	"strings"
	"testing"
	"time"
)

var agreeQueued = time.Date(2026, 9, 24, 16, 57, 0, 0, time.UTC)

// ran builds a completed event whose stamps are then moved by the caller, so
// each test says only what it is changing about a job that otherwise agrees
// with itself.
func ran(start, end time.Time) Event {
	return Event{Adapter: WebhookAdapter, DeliveryID: "d1", Phase: PhaseCompleted,
		JobID: 44021, RunID: 9001, RunAttempt: 1, Owner: "bsikar",
		Repository: "ra8-firmware", Workflow: "ci", JobName: "gates",
		CommitSHA: strings.Repeat("a", 40), Labels: []string{"self-hosted"},
		Conclusion: "success", QueuedAt: agreeQueued, StartedAt: start,
		CompletedAt: end, ObservedAt: observedAt}
}

// The shape every other test in this file varies has to pass on its own, or
// the refusals below prove nothing about the rule.
func TestAJobThatRanInOrderIsAccepted(t *testing.T) {
	event := ran(agreeQueued.Add(time.Minute), agreeQueued.Add(5*time.Minute))
	if err := event.Validate(); err != nil {
		t.Fatalf("a job queued, started and completed in order was refused: %v", err)
	}
}

func TestAStartBeforeTheQueueTimeIsRefused(t *testing.T) {
	event := ran(agreeQueued.Add(-time.Second), agreeQueued.Add(5*time.Minute))
	err := event.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a job that started before it was queued returned %v, want ErrInvalid", err)
	}
	if !strings.Contains(err.Error(), "started before queued") {
		t.Fatalf("the refusal does not name the disagreement: %v", err)
	}
}

func TestAnEndBeforeTheStartIsRefused(t *testing.T) {
	event := ran(agreeQueued.Add(5*time.Minute), agreeQueued.Add(time.Minute))
	err := event.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a job that completed before it started returned %v, want ErrInvalid", err)
	}
	if !strings.Contains(err.Error(), "completed before started") {
		t.Fatalf("the refusal does not name the disagreement: %v", err)
	}
}

// The one arm that was already stated keeps its exact wording, because a
// stored log line and an existing case in TestNormalizeRefusesUnusableDeliveries
// both read it.
func TestTheCompletedBeforeQueuedWordingIsUnchanged(t *testing.T) {
	event := ran(agreeQueued, agreeQueued.Add(-time.Second))
	err := event.Validate()
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "completed before queued") {
		t.Fatalf("completed before queued now reads %v", err)
	}
}

// Second-resolution stamps are what GitHub sends, so a job queued and started
// in the same second, or one that completed in the second it started, is
// ordinary rather than a disagreement. This is the boundary the rule must not
// creep past.
func TestEqualStampsAreNotADisagreement(t *testing.T) {
	for name, event := range map[string]Event{
		"started in the queue second":  ran(agreeQueued, agreeQueued.Add(time.Minute)),
		"completed in the same second": ran(agreeQueued.Add(time.Minute), agreeQueued.Add(time.Minute)),
		"all three at once":            ran(agreeQueued, agreeQueued),
	} {
		if err := event.Validate(); err != nil {
			t.Fatalf("%s was refused: %v", name, err)
		}
	}
}

// A zero stamp is absent, not early. Whether it is allowed to be absent is
// Validate's own question, asked before this rule: a queued event carries
// neither a start nor a completion and is accepted, and an in-progress event
// without a start is refused for the start being missing, not for its order.
func TestAnAbsentStampIsNotAnEarlyOne(t *testing.T) {
	queued := ran(time.Time{}, time.Time{})
	queued.Phase, queued.Conclusion = PhaseQueued, ""
	if err := queued.Validate(); err != nil {
		t.Fatalf("a queued event carrying no execution stamps was refused: %v", err)
	}
	running := ran(time.Time{}, time.Time{})
	running.Phase, running.Conclusion = PhaseInProgress, ""
	err := running.Validate()
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "start time") {
		t.Fatalf("a running event with no start returned %v, want the missing-start refusal", err)
	}
}

// The rule is judged on the whole delivery path, not just the struct: the
// stamps are payload fields, so a signed delivery stating them must be
// refused where it is normalized.
func TestADeliveryThatBackdatesItsStartIsRefused(t *testing.T) {
	success, early := "success", "2026-09-24T16:56:00Z"
	options := baseDelivery()
	options.action, options.status = "completed", "completed"
	options.conclusion = &success
	done := "2026-09-24T17:05:00Z"
	options.completedAt = &done
	options.startedAt = early
	_, err := NormalizeWorkflowJob(WebhookAdapter, "d1", payloadFor(t, options), observedAt)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a delivery whose job started before it was queued returned %v, want ErrInvalid", err)
	}
}

// The poll adapter copies both stamps off a snapshot verbatim and revalidates,
// so the same door is closed on the second way demand reaches this plane.
func TestAPollSnapshotCannotBackdateAStart(t *testing.T) {
	held := ran(time.Time{}, time.Time{})
	held.Phase, held.Conclusion = PhaseQueued, ""
	if err := held.Validate(); err != nil {
		t.Fatalf("the held event is not valid to begin with: %v", err)
	}
	_, err := held.observed(ReconcileAdapter, JobSnapshot{Phase: PhaseInProgress,
		StartedAt: agreeQueued.Add(-time.Minute)}, observedAt)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a snapshot that backdated the start returned %v, want ErrInvalid", err)
	}
}
