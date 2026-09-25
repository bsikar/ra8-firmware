package server

import (
	"errors"
	"math"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// finishedAt is the finish stamp of a spooled record, as a pointer, so a test
// can state an envelope in one line.
func finishedAt(at time.Time) *time.Time { return &at }

func TestAStatedEnvelopeIsAccepted(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	in, err := offlineInput(entry, cat)
	if err != nil {
		t.Fatalf("a stated envelope was refused: %v", err)
	}
	want := entry.FinishedAt.Sub(entry.StartedAt).Nanoseconds()
	if in.DurationNS != want {
		t.Fatalf("stored duration %d, envelope states %d", in.DurationNS, want)
	}
}

func TestARecordWithNoStartIsRefused(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	entry.StartedAt = time.Time{}
	in, err := offlineInput(entry, cat)
	if err == nil {
		t.Fatalf("a record stating no start was accepted: %+v", in)
	}
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("refusal does not travel as invalid: %v", err)
	}
}

// TestABlankStartCannotSaturateTheStoredDuration states the reason the refusal
// above exists rather than merely the refusal: subtracting the zero time
// saturates, so the alternative to refusing is writing math.MaxInt64
// nanoseconds to durable history as the length of the run.
func TestABlankStartCannotSaturateTheStoredDuration(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	entry.StartedAt = time.Time{}
	if saturated := entry.FinishedAt.Sub(entry.StartedAt).Nanoseconds(); saturated != math.MaxInt64 {
		t.Fatalf("premise of this test is gone: blank start subtracts to %d", saturated)
	}
	if in, err := offlineInput(entry, cat); err == nil {
		t.Fatalf("saturated duration %d was stored", in.DurationNS)
	}
}

func TestARecordWithNoFinishIsRefused(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	entry.FinishedAt = finishedAt(time.Time{})
	if in, err := offlineInput(entry, cat); err == nil {
		t.Fatalf("a record stating no finish was accepted: %+v", in)
	}
	entry.FinishedAt = nil
	if err := checkLocalEnvelopeIsStated(entry); err == nil {
		t.Fatal("a record carrying no finish stamp at all was accepted")
	}
}

func TestAnEnvelopeLongerThanAnyReviewedDeadlineIsRefused(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	entry.StartedAt = entry.FinishedAt.Add(-maxLocalEnvelope - time.Second)
	if in, err := offlineInput(entry, cat); err == nil {
		t.Fatalf("an envelope longer than any reviewed deadline was accepted: %+v", in)
	}
	if err := checkLocalEnvelopeIsStated(spool.Entry{
		StartedAt:  entry.FinishedAt.Add(-maxLocalEnvelope),
		FinishedAt: entry.FinishedAt,
	}); err != nil {
		t.Fatalf("an envelope exactly at the ceiling was refused: %v", err)
	}
	_ = cat
}

// TestTheCeilingLeavesTheLongestReviewedRunRoom pins the ceiling against the
// thing it is derived from: the catalog holds every task to a deadline of at
// most 86400 seconds, so a run that takes its whole deadline must still state
// an envelope this check accepts.
func TestTheCeilingLeavesTheLongestReviewedRunRoom(t *testing.T) {
	finish := time.Now().UTC()
	if err := checkLocalEnvelopeIsStated(spool.Entry{
		StartedAt:  finish.Add(-86400 * time.Second),
		FinishedAt: finishedAt(finish),
	}); err != nil {
		t.Fatalf("a run at the reviewed deadline ceiling was refused: %v", err)
	}
}

// TestTheStampsAreReadBeforeTheStepEvidence keeps the refusal ahead of the
// mapping: an unstated envelope is refused whatever the result says, rather
// than being downgraded to incomplete evidence and stored.
func TestTheStampsAreReadBeforeTheStepEvidence(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	entry.StartedAt = time.Time{}
	entry.Result.Steps = nil
	if in, err := offlineInput(entry, cat); err == nil {
		t.Fatalf("an unstated envelope was stored as %q", in.Result)
	}
}
