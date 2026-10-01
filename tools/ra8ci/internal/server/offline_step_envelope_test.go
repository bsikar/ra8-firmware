package server

import (
	"errors"
	"math"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// stepped hands back a record whose single step has been edited by mutate,
// so each test below states only the one thing it is about.
func stepped(t *testing.T, mutate func(*spool.Entry)) spool.Entry {
	t.Helper()
	entry, _ := offlineTestEntry(t)
	mutate(&entry)
	return entry
}

func TestAStatedStepIsAccepted(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	in, err := offlineInput(entry, cat)
	if err != nil {
		t.Fatalf("a stated step was refused: %v", err)
	}
	if len(in.Steps) != 1 || in.Steps[0].DurationNS != int64(entry.Result.Steps[0].Duration) {
		t.Fatalf("step not carried through: %+v", in.Steps)
	}
}

// TestAStepAtTheEnvelopeEdgesIsAccepted pins the boundary: a step running for
// exactly as long as the record it belongs to is honest, not suspicious.
func TestAStepAtTheEnvelopeEdgesIsAccepted(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].StartedAt = e.StartedAt
		e.Result.Steps[0].EndedAt = *e.FinishedAt
		e.Result.Steps[0].Duration = e.FinishedAt.Sub(e.StartedAt)
	})
	if _, err := offlineInput(entry, cat); err != nil {
		t.Fatalf("a step filling its record's envelope was refused: %v", err)
	}
}

func TestAStepWithNoStampsIsRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, c := range []struct {
		name   string
		mutate func(*spool.Entry)
	}{
		{"no start", func(e *spool.Entry) { e.Result.Steps[0].StartedAt = time.Time{} }},
		{"no end", func(e *spool.Entry) { e.Result.Steps[0].EndedAt = time.Time{} }},
	} {
		in, err := offlineInput(stepped(t, c.mutate), cat)
		if err == nil {
			t.Fatalf("a step stating %s was accepted: %+v", c.name, in.Steps)
		}
		if !errors.Is(err, store.ErrInvalid) {
			t.Fatalf("%s refusal does not travel as invalid: %v", c.name, err)
		}
	}
}

func TestAStepThatEndedBeforeItStartedIsRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].EndedAt = e.Result.Steps[0].StartedAt.Add(-time.Millisecond)
	})
	if in, err := offlineInput(entry, cat); err == nil {
		t.Fatalf("a step ending before it started was accepted: %+v", in.Steps)
	}
}

func TestAStepOutsideTheRecordEnvelopeIsRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, c := range []struct {
		name   string
		mutate func(*spool.Entry)
	}{
		{"starting before the record", func(e *spool.Entry) {
			e.Result.Steps[0].StartedAt = e.StartedAt.Add(-time.Second)
		}},
		{"ending after the record", func(e *spool.Entry) {
			e.Result.Steps[0].EndedAt = e.FinishedAt.Add(time.Second)
		}},
	} {
		if in, err := offlineInput(stepped(t, c.mutate), cat); err == nil {
			t.Fatalf("a step %s was accepted: %+v", c.name, in.Steps)
		}
	}
}

// TestAStepDurationCannotSaturateTheStoredStepDuration states the reason the
// duration bound exists rather than merely the refusal: the stated duration is
// copied into durable history verbatim, so the alternative to refusing is
// writing math.MaxInt64 nanoseconds as the length of one step of a run whose
// whole envelope is a second.
func TestAStepDurationCannotSaturateTheStoredStepDuration(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].Duration = time.Duration(math.MaxInt64)
	})
	in, err := offlineInput(entry, cat)
	if err == nil {
		t.Fatalf("a saturated step duration was stored: %+v", in.Steps)
	}
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("refusal does not travel as invalid: %v", err)
	}
}

func TestAStepDurationOutsideTheEnvelopeIsRefused(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, c := range []struct {
		name  string
		state func(*spool.Entry) time.Duration
	}{
		{"negative", func(e *spool.Entry) time.Duration { return -time.Nanosecond }},
		{"longer than the record", func(e *spool.Entry) time.Duration {
			return e.FinishedAt.Sub(e.StartedAt) + time.Nanosecond
		}},
	} {
		entry := stepped(t, func(e *spool.Entry) { e.Result.Steps[0].Duration = c.state(e) })
		if in, err := offlineInput(entry, cat); err == nil {
			t.Fatalf("a %s step duration was accepted: %+v", c.name, in.Steps)
		}
	}
}

// TestAnUnstatedStepIsRefusedRatherThanDowngraded separates this rule from the
// verdict the run already carries. An attempt whose steps prove nothing is
// downgraded to incomplete_evidence, and that downgrade writes the step rows
// anyway; the refusal is what keeps an uninterpretable step out of history.
func TestAnUnstatedStepIsRefusedRatherThanDowngraded(t *testing.T) {
	_, cat := offlineTestEntry(t)
	entry := stepped(t, func(e *spool.Entry) {
		e.Result.Steps[0].ExitCode = 1
		e.Result.Steps[0].StartedAt = time.Time{}
	})
	in, err := offlineInput(entry, cat)
	if err == nil {
		t.Fatalf("an unstated step was downgraded instead of refused: %+v", in)
	}
}
