// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"bytes"
	"errors"
	"strings"
	"testing"
	"time"
)

// Every message in this package arrives as client JSON, so each field is its
// own claim and the first door has to refuse the shapes no later rule reads.
// These hold that door: the decoder handed nothing to decode, an
// acknowledgement whose fencing fields or digests are not what a grant issued,
// a receipt whose outcome word and flags disagree, a step field no measurement
// could produce, and the host snapshot taken on the near side of the attempt.

const (
	refusedAssignment = "018f4c2a-7b1c-7def-8a90-1234567890ab"
	refusedAttempt    = "018f4c2a-7b1c-7def-8a91-1234567890ab"
)

func soundAck() Ack {
	return Ack{SchemaVersion: Version, AssignmentID: refusedAssignment, AttemptID: refusedAttempt,
		AssignmentVersion: 1, FencingToken: 1, CatalogSHA256: strings.Repeat("a", 64),
		SourceSnapshotSHA256: strings.Repeat("b", 64), HostFacts: sampleFacts()}
}

// A nil reader and a nil target are the two ways a caller asks this decoder to
// read nothing. Refusing them by name keeps the refusal on this side rather
// than letting a nil dereference reach the agent as a crash.
func TestDecodingIntoNothingIsRefusedRatherThanAttempted(t *testing.T) {
	var target Ack
	if err := DecodeStrict(nil, &target); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a nil reader was accepted: %v", err)
	}
	if err := DecodeStrict(bytes.NewReader([]byte(`{}`)), nil); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a nil target was accepted: %v", err)
	}
}

// The acknowledgement is what binds an agent's work to the grant it was issued
// under, so each of the four fencing fields and both digests is refused on its
// own. Accepting any of them would file the attempt against a grant the plane
// never issued.
func TestAnAcknowledgementNotBoundToAGrantIsRefused(t *testing.T) {
	if err := soundAck().Validate(); err != nil {
		t.Fatalf("a sound acknowledgement was refused: %v", err)
	}
	for name, broken := range map[string]func(*Ack){
		"a schema this plane does not speak": func(a *Ack) { a.SchemaVersion = Version + 1 },
		"no assignment":                      func(a *Ack) { a.AssignmentID = "" },
		"an assignment that is not a v7 id":  func(a *Ack) { a.AssignmentID = strings.Repeat("z", 36) },
		"no attempt":                         func(a *Ack) { a.AttemptID = "" },
		"an unversioned assignment":          func(a *Ack) { a.AssignmentVersion = 0 },
		"no fencing token":                   func(a *Ack) { a.FencingToken = 0 },
		"a catalog digest that is not one":   func(a *Ack) { a.CatalogSHA256 = "not a digest" },
		"no source snapshot digest":          func(a *Ack) { a.SourceSnapshotSHA256 = "" },
	} {
		ack := soundAck()
		broken(&ack)
		if !errors.Is(ack.Validate(), ErrInvalid) {
			t.Fatalf("%s was accepted", name)
		}
	}
}

// The outcome word and the flags beside it are two statements about the same
// ending, and a receipt that makes them disagree is not a reading of anything.
// An unknown word is refused outright rather than falling through to the
// gentlest branch, which is what a plane reading a newer agent's vocabulary
// would otherwise do.
func TestAnOutcomeTheFlagsDoNotSupportIsRefused(t *testing.T) {
	for name, receipt := range map[string]TerminalReceipt{
		"timed out without timing out": func() TerminalReceipt {
			r := stepless("timed_out", false)
			r.TimedOut = false
			return r
		}(),
		"timed out and cancelled at once": func() TerminalReceipt {
			r := stepless("timed_out", false)
			r.Cancelled = true
			return r
		}(),
		"cancelled without cancelling": func() TerminalReceipt {
			r := stepless("cancelled", false)
			r.Cancelled = false
			return r
		}(),
		"cancelled and timed out at once": func() TerminalReceipt {
			r := stepless("cancelled", false)
			r.TimedOut = true
			return r
		}(),
		"a word this plane does not know": func() TerminalReceipt {
			r := stepless("failed", false)
			r.Outcome = "abandoned"
			return r
		}(),
	} {
		if !errors.Is(receipt.Validate(), ErrInvalid) {
			t.Fatalf("%s was accepted", name)
		}
	}
	// And the pair that does agree still passes, so the refusals above are
	// about the disagreement rather than about the words.
	for _, outcome := range []string{"timed_out", "cancelled"} {
		receipt := stepless(outcome, false)
		if err := receipt.Validate(); err != nil {
			t.Fatalf("a coherent %s receipt was refused: %v", outcome, err)
		}
	}
}

// A step's own fields are judged before any rule compares one step with
// another, because a negative byte count or a stamp that was never taken makes
// every later comparison meaningless rather than wrong.
func TestAStepFieldNoMeasurementCouldProduceIsRefused(t *testing.T) {
	for name, broken := range map[string]func(*StepSummary){
		"a step with no name":       func(s *StepSummary) { s.Name = "" },
		"a step that never started": func(s *StepSummary) { s.StartedAt = time.Time{} },
		"a step that ended before it began": func(s *StepSummary) {
			s.EndedAt = s.StartedAt.Add(-time.Second)
		},
		"a negative duration":   func(s *StepSummary) { s.DurationNS = -1 },
		"negative bytes on out": func(s *StepSummary) { s.StdoutBytes = -1 },
		"negative bytes on err": func(s *StepSummary) { s.StderrBytes = -1 },
	} {
		receipt := outcomeReceipt(t)
		broken(&receipt.Steps[0])
		if !errors.Is(receipt.Validate(), ErrInvalid) {
			t.Fatalf("%s was accepted", name)
		}
	}
}

// Both host snapshots are judged on their own terms before either is compared
// with the attempt, and the start one is read first: a receipt carrying two
// unusable snapshots should name the earlier reading, which is the one an
// operator looks at first.
func TestAnUnusableHostSnapshotIsRefusedOnEitherSide(t *testing.T) {
	for name, broken := range map[string]func(*TerminalReceipt){
		"the start snapshot": func(r *TerminalReceipt) { r.HostFactsAtStart.Cores = 0 },
		"the end snapshot":   func(r *TerminalReceipt) { r.HostFactsAtEnd.Cores = 0 },
		"both":               func(r *TerminalReceipt) { r.HostFactsAtStart.Cores = 0; r.HostFactsAtEnd.Cores = 0 },
	} {
		receipt := outcomeReceipt(t)
		broken(&receipt)
		if !errors.Is(receipt.Validate(), ErrInvalid) {
			t.Fatalf("a receipt carrying %s unusable was accepted", name)
		}
	}
}

// The attempt rule reads the span and the reported duration independently,
// because they arrive as separate numbers in client JSON. Validate cannot show
// this: the duration rule ahead of it refuses a duration longer than its own
// span, so a duration past the ceiling with a span inside it only reaches here
// when the rule is asked directly, which is how the store asks it.
func TestADurationPastTheCeilingIsRefusedEvenInsideAShortSpan(t *testing.T) {
	receipt := attemptSpanReceipt(t)
	receipt.DurationNS = (maxAttemptSpan + clockDisagreement).Nanoseconds() + 1
	err := checkAttemptFitsADeadlineAGrantCouldIssue(receipt)
	if err == nil || !strings.Contains(err.Error(), "longer than the") {
		t.Fatalf("error = %v, want the reported duration refused on its own", err)
	}
	if !strings.Contains(err.Error(), "ns") {
		t.Fatalf("the refusal does not name the duration it read: %v", err)
	}
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("refusal is not ErrInvalid: %v", err)
	}
}
