// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"errors"
	"strings"
	"testing"
	"time"
)

var phaseQueuedAt = time.Date(2026, 9, 24, 16, 57, 0, 0, time.UTC)

// atPhase builds an otherwise valid event at one phase, with the stamps that
// phase earns and nothing else, so each test below says only what it adds.
func atPhase(phase Phase) Event {
	event := Event{Adapter: WebhookAdapter, DeliveryID: "d1", Phase: phase,
		JobID: 44021, RunID: 9001, RunAttempt: 1, Owner: "bsikar",
		Repository: "ra8-firmware", Workflow: "ci", JobName: "gates",
		CommitSHA: strings.Repeat("a", 40), Labels: []string{"self-hosted"},
		QueuedAt: phaseQueuedAt, ObservedAt: phaseQueuedAt.Add(10 * time.Minute)}
	if phase != PhaseQueued {
		event.StartedAt = phaseQueuedAt.Add(time.Minute)
	}
	if phase == PhaseCompleted {
		event.Conclusion = "success"
		event.CompletedAt = phaseQueuedAt.Add(5 * time.Minute)
	}
	return event
}

// The shapes every refusal below is a mutation of have to pass on their own.
func TestEachPhaseCarryingExactlyItsOwnStampsIsAccepted(t *testing.T) {
	for _, phase := range []Phase{PhaseQueued, PhaseInProgress, PhaseCompleted} {
		if err := atPhase(phase).Validate(); err != nil {
			t.Fatalf("a well-formed %s event was refused: %v", phase, err)
		}
	}
}

func TestAStampAheadOfItsPhaseIsRefused(t *testing.T) {
	started := phaseQueuedAt.Add(time.Minute)
	completed := phaseQueuedAt.Add(5 * time.Minute)
	for name, mutate := range map[string]func(*Event){
		"queued with a start":           func(e *Event) { e.StartedAt = started },
		"queued with a completion":      func(e *Event) { e.CompletedAt = completed },
		"queued with both":              func(e *Event) { e.StartedAt, e.CompletedAt = started, completed },
		"in progress with a completion": func(e *Event) { e.CompletedAt = completed },
	} {
		event := atPhase(PhaseQueued)
		if strings.HasPrefix(name, "in progress") {
			event = atPhase(PhaseInProgress)
		}
		mutate(&event)
		if err := event.Validate(); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s returned %v, want ErrInvalid", name, err)
		}
	}
}

// The conclusion arm of this rule has been stated since the package was
// written. The stamp arm has to refuse in exactly the same places, or one
// column of the same claim is fenced and the others are not.
func TestTheStampRuleCoversWhatTheConclusionRuleAlreadyCovered(t *testing.T) {
	for _, phase := range []Phase{PhaseQueued, PhaseInProgress} {
		withConclusion := atPhase(phase)
		withConclusion.Conclusion = "success"
		withStamp := atPhase(phase)
		withStamp.CompletedAt = phaseQueuedAt.Add(5 * time.Minute)
		conclusionRefused := errors.Is(withConclusion.Validate(), ErrInvalid)
		stampRefused := errors.Is(withStamp.Validate(), ErrInvalid)
		if conclusionRefused != stampRefused {
			t.Errorf("at %s the conclusion and the completion time disagree about the same claim: "+
				"conclusion refused %v, completion time refused %v", phase, conclusionRefused, stampRefused)
		}
	}
}

// A missing stamp is absence, and whether absence is allowed is Validate's
// own question. This rule must not answer it a second time or differently.
func TestAMissingStampIsStillValidatesOwnQuestion(t *testing.T) {
	noStart := atPhase(PhaseInProgress)
	noStart.StartedAt = time.Time{}
	if err := noStart.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a running job with no start returned %v, want ErrInvalid", err)
	}
	noCompletion := atPhase(PhaseCompleted)
	noCompletion.CompletedAt = time.Time{}
	if err := noCompletion.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a finished job with no completion returned %v, want ErrInvalid", err)
	}
	if err := checkStampsMatchThePhase(noStart); err != nil {
		t.Errorf("the phase rule answered a question about a missing stamp: %v", err)
	}
	if err := checkStampsMatchThePhase(noCompletion); err != nil {
		t.Errorf("the phase rule answered a question about a missing stamp: %v", err)
	}
}

// The webhook door: the normalizer reads completed_at whatever the action
// says, so a queued delivery carrying one has to be refused at the front.
func TestAQueuedDeliveryCarryingACompletionIsRefused(t *testing.T) {
	completed := "2026-09-24T17:05:00Z"
	options := baseDelivery()
	options.completedAt = &completed
	if _, err := NormalizeWorkflowJob(WebhookAdapter, "d1", payloadFor(t, options), observedAt); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a queued delivery with a completion time returned %v, want ErrInvalid", err)
	}
	if _, err := NormalizeWorkflowJob(WebhookAdapter, "d1", payloadFor(t, baseDelivery()), observedAt); err != nil {
		t.Fatalf("an ordinary queued delivery was refused: %v", err)
	}
}

// The poll door: observed() copies both snapshot stamps onto the event for
// whichever phase the forge answered with.
func TestAPollAnsweringAnEarlierPhaseCannotCarryALaterStamp(t *testing.T) {
	held := atPhase(PhaseInProgress)
	now := phaseQueuedAt.Add(30 * time.Minute)
	if _, err := held.observed(ReconcileAdapter, JobSnapshot{Phase: PhaseQueued,
		StartedAt: phaseQueuedAt.Add(time.Minute)}, now); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a queued snapshot carrying a start returned %v, want ErrInvalid", err)
	}
	if _, err := held.observed(ReconcileAdapter, JobSnapshot{Phase: PhaseInProgress,
		StartedAt: phaseQueuedAt.Add(time.Minute), CompletedAt: phaseQueuedAt.Add(5 * time.Minute)},
		now); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a running snapshot carrying a completion returned %v, want ErrInvalid", err)
	}
	if _, err := held.observed(ReconcileAdapter, JobSnapshot{Phase: PhaseQueued}, now); err != nil {
		t.Fatalf("an ordinary queued snapshot was refused: %v", err)
	}
}

// The pass's own conclusion path writes a completed event, which is the one
// phase that carries both stamps. It must still go through.
func TestConcludedDemandStillValidates(t *testing.T) {
	held := atPhase(PhaseQueued)
	next, err := held.concluded(ReconcileAdapter, phaseQueuedAt.Add(2*time.Hour))
	if err != nil {
		t.Fatalf("concluding forgotten demand was refused: %v", err)
	}
	if next.StartedAt.IsZero() || next.CompletedAt.IsZero() {
		t.Fatalf("a concluded event lost a stamp it needs: %+v", next)
	}
}

// The two time rules are separate questions asked in a fixed order, and both
// have to be reachable: a stamp ahead of its phase is refused even when the
// stamps are in order with each other, and an out-of-order pair is still
// refused at the phase that carries both.
func TestTheTwoTimeRulesAreBothReachable(t *testing.T) {
	ahead := atPhase(PhaseQueued)
	ahead.CompletedAt = phaseQueuedAt.Add(5 * time.Minute)
	if err := checkTimestampsAgree(ahead); err != nil {
		t.Fatalf("the ordering rule refused stamps that are in order: %v", err)
	}
	if err := ahead.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a stamp ahead of its phase was accepted: %v", err)
	}
	backwards := atPhase(PhaseCompleted)
	backwards.CompletedAt = phaseQueuedAt.Add(-time.Minute)
	if err := checkStampsMatchThePhase(backwards); err != nil {
		t.Fatalf("the phase rule answered an ordering question: %v", err)
	}
	if err := backwards.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a completion before the queue time was accepted: %v", err)
	}
}
