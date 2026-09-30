// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A spooled record states what happened on someone's laptop, and offlineInput
// turns that into the verdict durable history will hold. The mapping had been
// driven down its happy path and its metadata refusals, but the verdict itself
// had not: only "succeeded" and the missing-step downgrade were reached, so
// the four other words the switch can write, and the three refusals the
// stamps and the step list earn, were never the answer to anything.
//
// The verdict is the part a later reader quotes. A record that timed out and
// is filed as failed, or one cancelled and filed as succeeded, is not a
// cosmetic difference: it is history disagreeing with what the machine did.

// verdictOf maps a mutated record and expects it to be accepted, reporting
// the verdict it earned.
func verdictOf(t *testing.T, mutate func(*spool.Entry)) string {
	t.Helper()
	entry, cat := offlineTestEntry(t)
	mutate(&entry)
	in, err := offlineInput(entry, cat)
	if err != nil {
		t.Fatalf("the record was refused rather than judged: %v", err)
	}
	return in.Result
}

// refusedRecord maps a mutated record and expects the refusal.
func refusedRecord(t *testing.T, mutate func(*spool.Entry)) error {
	t.Helper()
	entry, cat := offlineTestEntry(t)
	mutate(&entry)
	in, err := offlineInput(entry, cat)
	if err == nil {
		t.Fatalf("the record was accepted rather than refused: %+v", in)
	}
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("the refusal is not the store's own: %v", err)
	}
	return err
}

func TestTheVerdictASpooledRecordEarns(t *testing.T) {
	// Order matters here and the switch states it: an executor that failed to
	// report is read before a timeout, a timeout before a cancellation, and
	// only a record that reported cleanly is judged on its exit code. Each
	// case below sets exactly the one fact its arm turns on, so an arm moved
	// above another would change an answer.
	for _, one := range []struct {
		what    string
		mutate  func(*spool.Entry)
		verdict string
	}{
		{"an executor that reported an error of its own", func(e *spool.Entry) {
			e.Error = "executor exited before the child did"
		}, "incomplete_evidence"},
		{"a result stating no start", func(e *spool.Entry) {
			e.Result.StartedAt = time.Time{}
		}, "incomplete_evidence"},
		{"a result stating no end", func(e *spool.Entry) {
			e.Result.EndedAt = time.Time{}
		}, "incomplete_evidence"},
		{"a run the deadline caught", func(e *spool.Entry) {
			e.Result.TimedOut = true
		}, "timed_out"},
		{"a run someone cancelled", func(e *spool.Entry) {
			e.Result.Cancelled = true
		}, "cancelled"},
		{"a child that exited non-zero", func(e *spool.Entry) {
			e.Result.ExitCode = 1
			e.Result.Steps[0].ExitCode = 1
		}, "failed"},
		{"a child that exited cleanly", func(e *spool.Entry) {}, "succeeded"},
	} {
		t.Run(one.what, func(t *testing.T) {
			if verdict := verdictOf(t, one.mutate); verdict != one.verdict {
				t.Fatalf("%s was filed as %q, want %q", one.what, verdict, one.verdict)
			}
		})
	}
}

func TestATimeoutIsReadBeforeACancellation(t *testing.T) {
	// A cancelled run and a timed-out run look alike from the outside: both
	// end without the child deciding. The executor sets both flags when a
	// deadline cancels the context, so the pair arrives together and the
	// order is what decides which word history keeps.
	verdict := verdictOf(t, func(e *spool.Entry) {
		e.Result.TimedOut = true
		e.Result.Cancelled = true
	})
	if verdict != "timed_out" {
		t.Fatalf("a run that both timed out and was cancelled was filed as %q, want timed_out", verdict)
	}
}

func TestAnExecutorErrorIsReadBeforeEitherOfThem(t *testing.T) {
	// An executor that could not report is not evidence of a timeout, even
	// when it also set the flag: the record cannot be trusted about either.
	verdict := verdictOf(t, func(e *spool.Entry) {
		e.Error = "executor exited before the child did"
		e.Result.TimedOut = true
		e.Result.Cancelled = true
	})
	if verdict != "incomplete_evidence" {
		t.Fatalf("a record its own executor could not vouch for was filed as %q, want incomplete_evidence", verdict)
	}
}

func TestASucceededRunIsDowngradedByAStepThatDidNot(t *testing.T) {
	// The run-level exit code is the child's last word, and a task's steps
	// run under one child, so a zero at the top with a step that failed
	// beneath it is a record disagreeing with itself. It is downgraded rather
	// than refused: the evidence is real, it just does not add up to a pass.
	for _, one := range []struct {
		what   string
		mutate func(*executor.StepResult)
	}{
		{"a step that exited non-zero", func(s *executor.StepResult) { s.ExitCode = 1 }},
		{"a step the deadline caught", func(s *executor.StepResult) { s.TimedOut = true }},
		{"a step someone cancelled", func(s *executor.StepResult) { s.Cancelled = true }},
	} {
		t.Run(one.what, func(t *testing.T) {
			verdict := verdictOf(t, func(e *spool.Entry) {
				one.mutate(&e.Result.Steps[0])
			})
			if verdict != "incomplete_evidence" {
				t.Fatalf("a clean run over %s was filed as %q, want incomplete_evidence", one.what, verdict)
			}
		})
	}
}

func TestAResultRunningOutsideTheRecordsEnvelopeIsRefused(t *testing.T) {
	// The record's own stamps are the envelope; the executor's are what it
	// measured inside them. An executor claiming to have started before the
	// record did, or ended after it finished, is stating something the record
	// cannot cover, and the span between them is what lands in duration_ns.
	for _, one := range []struct {
		what   string
		mutate func(*spool.Entry)
	}{
		{"an executor that started before the record did", func(e *spool.Entry) {
			e.Result.StartedAt = e.StartedAt.Add(-time.Second)
		}},
		{"an executor that ended after the record finished", func(e *spool.Entry) {
			e.Result.EndedAt = e.FinishedAt.Add(time.Second)
		}},
	} {
		t.Run(one.what, func(t *testing.T) {
			err := refusedRecord(t, one.mutate)
			if !strings.Contains(err.Error(), "executor timestamps outside local envelope") {
				t.Fatalf("the refusal does not say what is wrong: %v", err)
			}
		})
	}
}

func TestMoreStepsThanTheTaskDeclaresAreRefused(t *testing.T) {
	// The step list is read positionally against the reviewed definition, so
	// a record carrying more steps than the task has is refused before the
	// loop rather than indexed past the end of the definition.
	refusedRecord(t, func(e *spool.Entry) {
		e.Result.Steps = append(e.Result.Steps, e.Result.Steps[0])
	})
}

func TestAStepUnderAnotherStepsNameIsRefused(t *testing.T) {
	// Position alone is not identity. A record whose steps are the right
	// shape but the wrong names would file one step's evidence under
	// another's key, and the key is what a later reader joins on.
	refusedRecord(t, func(e *spool.Entry) {
		e.Result.Steps[0].Name = e.Result.Steps[0].Name + "-renamed"
	})
}

func TestARecordFinishingInTheFutureIsRefused(t *testing.T) {
	// Five minutes of slack covers skew between the laptop that stamped the
	// record and the plane that reads it. Past that, the record is describing
	// something that has not happened, and filing it would put a finish time
	// in history that later runs sort against.
	refusedRecord(t, func(e *spool.Entry) {
		ahead := time.Now().UTC().Add(10 * time.Minute)
		e.FinishedAt = &ahead
	})
}

func TestARecordFinishingBeforeItStartedIsRefused(t *testing.T) {
	// The envelope check ahead of this one bounds how long a record may span
	// but not which way round it runs, because a negative span is not a long
	// one. The steps are cleared so the refusal is this rule's rather than a
	// step's: a step inside a backwards envelope is outside it by definition.
	refusedRecord(t, func(e *spool.Entry) {
		e.StartedAt = e.FinishedAt.Add(time.Second)
		e.Result.StartedAt = time.Time{}
		e.Result.EndedAt = time.Time{}
		e.Result.Steps = nil
	})
}
