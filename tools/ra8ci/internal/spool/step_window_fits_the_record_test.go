// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// stepped builds a terminal record whose envelope is a minute wide and whose
// steps sit inside it, the shape a run this package wrote produces.
func stepped(steps ...executor.StepResult) Entry {
	started := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	finished := started.Add(time.Minute)
	return Entry{StartedAt: started, FinishedAt: &finished,
		Result: &executor.Result{StartedAt: started, EndedAt: finished, Steps: steps}}
}

// insideStep is a step that ran in the second half of that envelope.
func insideStep(name string) executor.StepResult {
	started := time.Date(2026, 9, 26, 12, 0, 30, 0, time.UTC)
	return executor.StepResult{Name: name, StartedAt: started,
		EndedAt: started.Add(10 * time.Second), Duration: 10 * time.Second}
}

// The steps an honest run produces are inside the envelope by construction,
// so the door they now pass through has to leave them alone.
func TestStepsInsideTheRecordPass(t *testing.T) {
	entry := stepped(insideStep("build"), insideStep("test"))
	if err := checkStepWindowsFitTheRecord(entry); err != nil {
		t.Fatalf("honest steps were refused: %v", err)
	}
	if err := checkTerminalRecordCarriesItsRun(entry); err != nil {
		t.Fatalf("the record carrying them was refused: %v", err)
	}
}

// A record with no steps at all states nothing for this rule to judge. The
// ingest end accepts one (an incomplete_evidence upload carries no step rows),
// so refusing it here would refuse a record the server would have taken.
func TestARecordWithoutStepsIsNotRefused(t *testing.T) {
	if err := checkStepWindowsFitTheRecord(stepped()); err != nil {
		t.Fatalf("a record stating no steps was refused: %v", err)
	}
	entry := stepped()
	entry.Result = nil
	if err := checkStepWindowsFitTheRecord(entry); err != nil {
		t.Fatalf("a record stating no result was refused by the step rule: %v", err)
	}
}

// The four shapes the ingest end refuses a step on, each judged from the
// record alone, so each is refused on every upload from every host forever.
func TestAStepTheIngestEndCanNeverAcceptIsRefused(t *testing.T) {
	envelopeStart := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	for name, step := range map[string]executor.StepResult{
		"no start": {Name: "build", EndedAt: envelopeStart.Add(time.Second)},
		"no end":   {Name: "build", StartedAt: envelopeStart},
		"ended before it started": {Name: "build", StartedAt: envelopeStart.Add(20 * time.Second),
			EndedAt: envelopeStart.Add(10 * time.Second)},
		"began before the record": {Name: "build", StartedAt: envelopeStart.Add(-time.Second),
			EndedAt: envelopeStart.Add(time.Second)},
		"ended after the record": {Name: "build", StartedAt: envelopeStart.Add(30 * time.Second),
			EndedAt: envelopeStart.Add(2 * time.Minute)},
		"negative duration": {Name: "build", StartedAt: envelopeStart, EndedAt: envelopeStart,
			Duration: -time.Nanosecond},
		"duration longer than the envelope": {Name: "build", StartedAt: envelopeStart,
			EndedAt: envelopeStart.Add(time.Second), Duration: time.Hour},
	} {
		err := checkStepWindowsFitTheRecord(stepped(step))
		if err == nil {
			t.Fatalf("%s: accepted", name)
		}
		if !errors.Is(err, errStepOutsideTheRecord) {
			t.Fatalf("%s: refusal did not carry the rule's error: %v", name, err)
		}
		if err := checkTerminalRecordCarriesItsRun(stepped(step)); !errors.Is(err, errRecordCannotBeUploaded) {
			t.Fatalf("%s: the record door did not refuse it: %v", name, err)
		}
	}
}

// Transcribed from server.checkLocalStepIsStated: the same shapes judged the
// same way. If the ingest end changes its mind about what a step must state,
// this fails rather than this door quietly refusing more than it should.
func TestThisStepRuleRefusesWhatTheIngestEndRefuses(t *testing.T) {
	ingestWouldRefuse := func(entry Entry, step executor.StepResult) bool {
		switch {
		case step.StartedAt.IsZero() || step.EndedAt.IsZero():
			return true
		case step.EndedAt.Before(step.StartedAt):
			return true
		case step.StartedAt.Before(entry.StartedAt) || step.EndedAt.After(*entry.FinishedAt):
			return true
		}
		envelope := entry.FinishedAt.Sub(entry.StartedAt)
		return step.Duration < 0 || step.Duration > envelope
	}
	start := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	candidates := []executor.StepResult{
		insideStep("build"),
		{Name: "build", StartedAt: start, EndedAt: start.Add(time.Minute), Duration: time.Minute},
		{Name: "build", StartedAt: start, EndedAt: start.Add(time.Minute), Duration: time.Minute + 1},
		{Name: "build", StartedAt: start.Add(-time.Nanosecond), EndedAt: start.Add(time.Second)},
		{Name: "build", StartedAt: start, EndedAt: start.Add(time.Minute + time.Nanosecond)},
		{Name: "build", StartedAt: start.Add(time.Second), EndedAt: start},
		{Name: "build"},
		{Name: "build", StartedAt: start},
		{Name: "build", StartedAt: start, EndedAt: start, Duration: -1},
	}
	for i, step := range candidates {
		entry := stepped(step)
		refusedHere := checkStepWindowsFitTheRecord(entry) != nil
		if refusedHere != ingestWouldRefuse(entry, step) {
			t.Fatalf("step %d: refused here %v, ingest refuses %v", i, refusedHere, ingestWouldRefuse(entry, step))
		}
	}
}

// One nanosecond either side of each boundary, so the rule is not passing by
// luck of a coarse fixture.
func TestTheStepBoundariesAreExact(t *testing.T) {
	start := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	finish := start.Add(time.Minute)
	exact := executor.StepResult{Name: "build", StartedAt: start, EndedAt: finish, Duration: time.Minute}
	if err := checkStepWindowsFitTheRecord(stepped(exact)); err != nil {
		t.Fatalf("a step filling the envelope exactly was refused: %v", err)
	}
	early := exact
	early.StartedAt = start.Add(-time.Nanosecond)
	if checkStepWindowsFitTheRecord(stepped(early)) == nil {
		t.Fatal("a step beginning one nanosecond before the record was accepted")
	}
	late := exact
	late.EndedAt = finish.Add(time.Nanosecond)
	if checkStepWindowsFitTheRecord(stepped(late)) == nil {
		t.Fatal("a step ending one nanosecond after the record was accepted")
	}
	long := exact
	long.Duration = time.Minute + time.Nanosecond
	if checkStepWindowsFitTheRecord(stepped(long)) == nil {
		t.Fatal("a duration one nanosecond longer than the envelope was accepted")
	}
}

// The refusal has to say which step, because the operator's next move is to
// look at it. The ordinal is stated whatever the name is, since a record
// edited into this state is exactly the one whose step names may be missing.
func TestTheStepRefusalNamesTheStep(t *testing.T) {
	named := executor.StepResult{Name: "hil-run"}
	err := checkStepWindowsFitTheRecord(stepped(insideStep("build"), named))
	if err == nil || !strings.Contains(err.Error(), `step 1 "hil-run"`) {
		t.Fatalf("refusal did not name the step: %v", err)
	}
	err = checkStepWindowsFitTheRecord(stepped(executor.StepResult{}))
	if err == nil || !strings.Contains(err.Error(), "step 0") {
		t.Fatalf("refusal did not state the ordinal of an unnamed step: %v", err)
	}
}

// A record whose own envelope is not stated cannot judge a step at all. The
// record-level rules refuse such a record first, so this only has to avoid
// reading through a nil finish stamp, and it says so rather than passing the
// step.
func TestAStepCannotBeJudgedAgainstAnUnstatedEnvelope(t *testing.T) {
	entry := stepped(insideStep("build"))
	entry.FinishedAt = nil
	err := checkStepWindowsFitTheRecord(entry)
	if err == nil || !errors.Is(err, errStepOutsideTheRecord) {
		t.Fatalf("a step judged against no envelope was accepted: %v", err)
	}
	if err := checkTerminalRecordCarriesItsRun(entry); !errors.Is(err, errStampsOutOfOrder) {
		t.Fatalf("the record door stopped naming the missing finish stamp: %v", err)
	}
	empty := stepped()
	empty.FinishedAt = nil
	if err := checkStepWindowsFitTheRecord(empty); err != nil {
		t.Fatalf("a record with no steps and no envelope was refused by the step rule: %v", err)
	}
}

// The door Pending actually reads from, on a record edited after the run. The
// sweep stops, names the file, and offers nothing past it, which is what this
// rule is worth over an unnamed HTTP 400.
func TestAStepOutsideTheRecordStopsTheSweepByName(t *testing.T) {
	s, entry := spooledRun(t)
	editTerminalRecord(t, s, entry, func(e *Entry) {
		e.Result.Steps = []executor.StepResult{{Name: "build",
			StartedAt: e.StartedAt.Add(-time.Hour), EndedAt: *e.FinishedAt}}
	})
	pending, err := s.Pending()
	if err == nil || len(pending) != 0 {
		t.Fatalf("a record with a step outside its envelope was offered: %+v %v", pending, err)
	}
	if !errors.Is(err, errStepOutsideTheRecord) || !errors.Is(err, errRecordCannotBeUploaded) {
		t.Fatalf("refusal did not carry both errors: %v", err)
	}
	if !strings.Contains(err.Error(), entry.ID+".finished.json") || !strings.Contains(err.Error(), "build") {
		t.Fatalf("refusal named neither the file nor the step: %v", err)
	}
	for pass := 0; pass < 3; pass++ {
		if _, err := s.Pending(); err == nil {
			t.Fatalf("pass %d silently dropped the record", pass)
		}
	}
}

// A run this package finishes with real steps still comes back from Pending,
// so the rule costs an honest disconnected host nothing.
func TestAFinishedRunWithStepsIsStillPending(t *testing.T) {
	s, entry := spooledRun(t)
	editTerminalRecord(t, s, entry, func(e *Entry) {
		e.Result.Steps = []executor.StepResult{{Name: "build", StartedAt: e.StartedAt,
			EndedAt: *e.FinishedAt, Duration: e.FinishedAt.Sub(e.StartedAt)}}
	})
	pending, err := s.Pending()
	if err != nil || len(pending) != 1 {
		t.Fatalf("an honest record with steps did not come back: %+v %v", pending, err)
	}
	if len(pending[0].Result.Steps) != 1 {
		t.Fatalf("the step was not carried through: %+v", pending[0].Result.Steps)
	}
}
