// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// What a finished attempt must state before the plane will record it.
//
// A terminal attempt result is the last thing anyone writes about a piece of
// work, and every later read trusts it: the run summary, the evidence
// accounting, the skip of everything downstream. So the shape is judged
// before a transaction opens, and the judgement is stricter than "name one of
// the six outcomes". An outcome has to agree with the facts beside it, and a
// result that disagrees with its own exit code or its own deadline flag is
// refused rather than stored and puzzled over later.
//
// These refusals land before the pool is touched, which is what lets a store
// with no database answer them at all.

func TestAFinishedAttemptIsRefusedWhenItsResultDisagreesWithItself(t *testing.T) {
	good, err := NewID()
	if err != nil {
		t.Fatalf("minting an identifier: %v", err)
	}
	zero, one := 0, 1

	for _, refusal := range []struct {
		name string
		in   FinishAttemptInput
	}{
		{
			// The identity half: an attempt nobody can name, and an
			// actor nobody can be held to.
			name: "a malformed attempt identifier",
			in:   FinishAttemptInput{AttemptID: "attempt-3", ActorID: "runner", Result: "failed"},
		},
		{
			name: "no actor to record the finish under",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "", Result: "failed"},
		},
		{
			name: "a reason past the bound",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "failed", Reason: strings.Repeat("x", 1025)},
		},
		{
			name: "no result at all",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: ""},
		},
		{
			name: "a result the machine has never heard of",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "exploded"},
		},
		{
			// A success is the only result that carries a burden of
			// proof, and all four parts of it are load-bearing.
			name: "a success with no exit code to show for it",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "succeeded", EvidenceComplete: true},
		},
		{
			name: "a success whose child failed",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "succeeded", ChildExitCode: &one, EvidenceComplete: true},
		},
		{
			name: "a success that ran out of time",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "succeeded", ChildExitCode: &zero, HitDeadline: true, EvidenceComplete: true},
		},
		{
			name: "a success whose evidence never arrived",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "succeeded", ChildExitCode: &zero},
		},
		{
			// The deadline flag is the fact that decides between
			// failed and timed_out, so neither may contradict it.
			name: "a failure that says it hit its deadline",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "failed", ChildExitCode: &one, HitDeadline: true},
		},
		{
			name: "a timeout that says it did not hit its deadline",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "timed_out"},
		},
		{
			name: "a cancellation that says it hit its deadline",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "cancelled", HitDeadline: true},
		},
		{
			name: "a preemption that says it hit its deadline",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "preempted", HitDeadline: true},
		},
		{
			name: "a lost attempt that says it hit its deadline",
			in:   FinishAttemptInput{AttemptID: good, ActorID: "runner", Result: "lost", HitDeadline: true},
		},
	} {
		t.Run(refusal.name, func(t *testing.T) {
			// A store with no database at all: reaching Postgres
			// would panic here, so arriving at an answer is itself
			// the proof that the refusal came first.
			err := (&Store{}).FinishAttempt(context.Background(), refusal.in)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("accepted %s: %v", refusal.name, err)
			}
		})
	}
}
