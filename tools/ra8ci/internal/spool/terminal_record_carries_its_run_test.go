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

// The honest record this package writes passes the door untouched, which is
// the only shape Finish can produce.
func TestAFinishedRecordCarriesItsRun(t *testing.T) {
	s, entry := spooledRun(t)
	if err := checkTerminalRecordCarriesItsRun(entry); err != nil {
		t.Fatalf("an honestly finished record was refused: %v", err)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 1 {
		t.Fatalf("honest record not pending: %+v %v", pending, err)
	}
}

// A result is what the upload reports. offlineInput refuses a record without
// one, so a file on disk missing it can never be ingested.
func TestATerminalRecordWithoutAResultIsRefused(t *testing.T) {
	s, entry := spooledRun(t)
	editTerminalRecord(t, s, entry, func(e *Entry) { e.Result = nil })
	pending, err := s.Pending()
	if err == nil || !strings.Contains(err.Error(), "no execution result") {
		t.Fatalf("a record with no result was offered for upload: %+v %v", pending, err)
	}
	if len(pending) != 0 {
		t.Fatalf("records offered past a refusal: %+v", pending)
	}
	if !errors.Is(err, errRecordCannotBeUploaded) {
		t.Fatalf("refusal did not carry the rule's error: %v", err)
	}
}

// The stamps rule Finish applies, applied again to what is on disk. A record
// written before that rule existed, or edited after the run, reaches this door
// with stamps Finish would never have written.
func TestStampsOutOfOrderOnDiskAreRefused(t *testing.T) {
	for name, change := range map[string]func(*Entry){
		"finish precedes start": func(e *Entry) {
			earlier := e.StartedAt.Add(-time.Minute)
			e.FinishedAt = &earlier
		},
		"execution began before the record": func(e *Entry) {
			e.Result.StartedAt = e.StartedAt.Add(-time.Second)
			e.Result.EndedAt = e.StartedAt
		},
		"execution ended after the record": func(e *Entry) {
			e.Result.StartedAt = e.StartedAt
			e.Result.EndedAt = e.FinishedAt.Add(time.Second)
		},
	} {
		s, entry := spooledRun(t)
		editTerminalRecord(t, s, entry, change)
		pending, err := s.Pending()
		if err == nil || !errors.Is(err, errStampsOutOfOrder) {
			t.Fatalf("%s: accepted: %+v %v", name, pending, err)
		}
		if !errors.Is(err, errRecordCannotBeUploaded) || len(pending) != 0 {
			t.Fatalf("%s: refusal did not stop the sweep: %+v %v", name, pending, err)
		}
	}
}

// The refusal has to name the file, because the operator's next move is to
// look at it: the point of refusing here rather than reading back an HTTP
// status with no field named.
func TestTheRefusalNamesTheRecordOnDisk(t *testing.T) {
	s, entry := spooledRun(t)
	editTerminalRecord(t, s, entry, func(e *Entry) { e.Result = nil })
	_, err := s.Pending()
	if err == nil || !strings.Contains(err.Error(), entry.ID+".finished.json") {
		t.Fatalf("refusal did not name the record: %v", err)
	}
}

// A skip would be worse than a refusal: Pending is the only thing that offers
// a record for upload, so a silently skipped record is evidence the server
// never receives and nobody is told about. This pins that the answer is a
// refusal and that it is the same on every pass.
func TestTheRefusalIsStableAndNothingIsSilentlyDropped(t *testing.T) {
	s, entry := spooledRun(t)
	editTerminalRecord(t, s, entry, func(e *Entry) { e.Result = nil })
	for pass := 0; pass < 3; pass++ {
		pending, err := s.Pending()
		if err == nil || len(pending) != 0 {
			t.Fatalf("pass %d: %+v %v", pass, pending, err)
		}
	}
}

// One unusable file stops the whole sweep, which is the cost this rule is
// weighed against: the refusal is deliberate, so a good record beside a bad
// one does not get uploaded either, and the operator is told which file to
// fix rather than left reading an unnamed 400 forever.
func TestOneUnusableRecordIsReportedRatherThanTheWholeOutboxStalling(t *testing.T) {
	s, entry := spooledRun(t)
	second, err := s.BeginWithMetadata("format-check", entry.CatalogDigest, Metadata{
		Source: SourceIdentity{Repository: entry.Source.Repository, Branch: entry.Source.Branch,
			CommitSHA: entry.Source.CommitSHA, Verification: entry.Source.Verification},
		Tier: entry.Tier, Scope: entry.Scope, DeadlineSeconds: entry.DeadlineSeconds,
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Finish(second, executor.Result{TaskName: "format-check"}, nil); err != nil {
		t.Fatal(err)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 2 {
		t.Fatalf("two honest records did not both come back: %+v %v", pending, err)
	}
	editTerminalRecord(t, s, entry, func(e *Entry) { e.Result = nil })
	pending, err = s.Pending()
	if err == nil || len(pending) != 0 {
		t.Fatalf("a record was offered past the unusable one: %+v %v", pending, err)
	}
	if !strings.Contains(err.Error(), entry.ID) {
		t.Fatalf("the refusal named the wrong record: %v", err)
	}
}

// Transcribed from server.offlineInput: the conditions it refuses a record on
// for the same two reasons, rebuilt here rather than called. If the ingest end
// changes its mind about what a terminal record must carry, this fails.
func TestThisDoorRefusesWhatTheIngestEndRefuses(t *testing.T) {
	ingestWouldRefuse := func(e Entry) bool {
		if e.FinishedAt == nil || e.Result == nil {
			return true
		}
		if e.StartedAt.After(*e.FinishedAt) {
			return true
		}
		return !e.Result.StartedAt.IsZero() &&
			(e.Result.StartedAt.Before(e.StartedAt) || e.Result.EndedAt.After(*e.FinishedAt))
	}
	base := func() Entry {
		started := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
		finished := started.Add(time.Minute)
		return Entry{StartedAt: started, FinishedAt: &finished,
			Result: &executor.Result{StartedAt: started, EndedAt: finished}}
	}
	changes := []func(*Entry){
		func(*Entry) {},
		func(e *Entry) { e.Result = nil },
		func(e *Entry) { e.Result.StartedAt = e.StartedAt.Add(-time.Nanosecond) },
		func(e *Entry) { e.Result.EndedAt = e.FinishedAt.Add(time.Nanosecond) },
		func(e *Entry) { earlier := e.StartedAt.Add(-time.Hour); e.FinishedAt = &earlier },
		func(e *Entry) { e.Result.StartedAt, e.Result.EndedAt = time.Time{}, time.Time{} },
		func(e *Entry) { e.Result.EndedAt = *e.FinishedAt },
	}
	for i, change := range changes {
		entry := base()
		change(&entry)
		refusedHere := checkTerminalRecordCarriesItsRun(entry) != nil
		if refusedHere != ingestWouldRefuse(entry) {
			t.Fatalf("shape %d: refused here %v, ingest refuses %v", i, refusedHere, ingestWouldRefuse(entry))
		}
	}
}

// One nanosecond either side of the boundary, so the rule is not passing by
// luck of a coarse fixture.
func TestTheBoundaryIsExact(t *testing.T) {
	started := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	finished := started.Add(time.Minute)
	entry := Entry{StartedAt: started, FinishedAt: &finished,
		Result: &executor.Result{StartedAt: started, EndedAt: finished}}
	if err := checkTerminalRecordCarriesItsRun(entry); err != nil {
		t.Fatalf("the exact envelope was refused: %v", err)
	}
	entry.Result.StartedAt = started.Add(-time.Nanosecond)
	if err := checkTerminalRecordCarriesItsRun(entry); err == nil {
		t.Fatal("one nanosecond before the record's start was accepted")
	}
	entry.Result.StartedAt = started
	entry.Result.EndedAt = finished.Add(time.Nanosecond)
	if err := checkTerminalRecordCarriesItsRun(entry); err == nil {
		t.Fatal("one nanosecond after the record's finish was accepted")
	}
}
