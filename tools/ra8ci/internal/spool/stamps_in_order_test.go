// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// stamped builds a terminal record with the stamps a caller would see after a
// run: the record's own window, and the executor's window inside it.
func stamped(start, finish, ran, ended time.Time) Entry {
	entry := Entry{SchemaVersion: 2, ID: "0123456789abcdef0123456789abcdef",
		Task: "format-check", StartedAt: start, SyncState: "unsynced"}
	if !finish.IsZero() {
		stop := finish
		entry.FinishedAt = &stop
	}
	entry.Result = &executor.Result{TaskName: "format-check", StartedAt: ran, EndedAt: ended}
	return entry
}

func TestStampsInOrderAcceptsEveryOrdinaryShape(t *testing.T) {
	base := time.Date(2026, 9, 26, 14, 0, 0, 0, time.UTC)
	for _, c := range []struct {
		name  string
		entry Entry
	}{
		{"an ordinary run", stamped(base, base.Add(90*time.Second), base.Add(time.Second), base.Add(80*time.Second))},
		{"a run shorter than the clock's resolution", stamped(base, base, base, base)},
		{"execution sharing both edges of the envelope", stamped(base, base.Add(time.Minute), base, base.Add(time.Minute))},
		{"a result that states no window", stamped(base, base.Add(time.Minute), time.Time{}, time.Time{})},
		{"a result that states only a start", stamped(base, base.Add(time.Minute), base.Add(time.Second), time.Time{})},
	} {
		if err := checkStampsAreInOrder(c.entry); err != nil {
			t.Errorf("%s: refused: %v", c.name, err)
		}
	}
}

func TestStampsInOrderAcceptsARecordThatCarriesNoResult(t *testing.T) {
	base := time.Date(2026, 9, 26, 14, 0, 0, 0, time.UTC)
	entry := stamped(base, base.Add(time.Minute), time.Time{}, time.Time{})
	entry.Result = nil
	if err := checkStampsAreInOrder(entry); err != nil {
		t.Fatalf("refused a record with no result: %v", err)
	}
}

func TestStampsInOrderRefusesAndNamesWhatMoved(t *testing.T) {
	base := time.Date(2026, 9, 26, 14, 0, 0, 0, time.UTC)
	for _, c := range []struct {
		name  string
		entry Entry
		says  string
	}{
		{"a clock that stepped back during the run",
			stamped(base, base.Add(-time.Second), base, base), "finish stamp"},
		{"a clock that stepped back by an hour",
			stamped(base, base.Add(-time.Hour), base, base), "finish stamp"},
		{"execution beginning before the record did",
			stamped(base, base.Add(time.Minute), base.Add(-time.Second), base.Add(time.Second)), "execution began"},
		{"execution ending after the record did",
			stamped(base, base.Add(time.Minute), base, base.Add(2*time.Minute)), "execution ended"},
	} {
		err := checkStampsAreInOrder(c.entry)
		if err == nil {
			t.Errorf("%s: accepted", c.name)
			continue
		}
		if !errors.Is(err, errStampsOutOfOrder) {
			t.Errorf("%s: wrong sentinel: %v", c.name, err)
		}
		if !strings.Contains(err.Error(), c.says) {
			t.Errorf("%s: refusal does not name %q: %v", c.name, c.says, err)
		}
	}
}

func TestStampsInOrderRefusesARecordWithNoFinishStamp(t *testing.T) {
	base := time.Date(2026, 9, 26, 14, 0, 0, 0, time.UTC)
	entry := stamped(base, time.Time{}, base, base)
	if err := checkStampsAreInOrder(entry); err == nil || !errors.Is(err, errStampsOutOfOrder) {
		t.Fatalf("accepted a record with no finish stamp: %v", err)
	}
}

// The rule is one-sided on purpose: a long run is ordinary, and the span a
// record may state is judged at ingest against the reviewed deadline, not here.
func TestALongRunIsOrdinary(t *testing.T) {
	base := time.Date(2026, 9, 26, 14, 0, 0, 0, time.UTC)
	entry := stamped(base, base.Add(20*time.Hour), base.Add(time.Second), base.Add(19*time.Hour))
	if err := checkStampsAreInOrder(entry); err != nil {
		t.Fatalf("refused a twenty-hour run: %v", err)
	}
}

// ingestWouldRefuse transcribes the two ingest rules this class of record has
// left to pass: offlineInput's executor-envelope refusal and its closing
// "start is not after finish", restated again by store.validateLocalRun as
// FinishedAt.Before(StartedAt). Nothing imports the server from here, so the
// rules are written out; the sweep below fails if the local refusal ever stops
// covering them and a record that cannot be ingested is written again.
func ingestWouldRefuse(entry Entry) bool {
	if entry.FinishedAt == nil {
		return true
	}
	if entry.StartedAt.After(*entry.FinishedAt) {
		return true
	}
	if entry.Result != nil && !entry.Result.StartedAt.IsZero() &&
		(entry.Result.StartedAt.Before(entry.StartedAt) || entry.Result.EndedAt.After(*entry.FinishedAt)) {
		return true
	}
	return false
}

func TestEveryRecordIngestWouldRefuseIsRefusedHere(t *testing.T) {
	base := time.Date(2026, 9, 26, 14, 0, 0, 0, time.UTC)
	offsets := []time.Duration{-time.Hour, -time.Second, 0, time.Second, time.Minute}
	checked := 0
	for _, finish := range offsets {
		for _, ran := range offsets {
			for _, ended := range offsets {
				entry := stamped(base, base.Add(finish), base.Add(ran), base.Add(ended))
				refusedHere := checkStampsAreInOrder(entry) != nil
				if ingestWouldRefuse(entry) && !refusedHere {
					t.Errorf("written locally, refused at ingest: finish %s ran %s ended %s",
						finish, ran, ended)
				}
				checked++
			}
		}
	}
	if checked != len(offsets)*len(offsets)*len(offsets) {
		t.Fatalf("swept %d shapes", checked)
	}
}

// refrozen replaces the start record on disk with one stating stamp, which is
// what a host whose clock steps backwards mid-run leaves behind: a frozen
// start later than every reading Finish can take.
func refrozen(t *testing.T, s *Spool, entry Entry, stamp time.Time) Entry {
	t.Helper()
	entry.StartedAt = stamp
	if err := os.Remove(filepath.Join(s.directory, entry.ID+".started.json")); err != nil {
		t.Fatal(err)
	}
	if err := s.write(entry.ID+".started.json", entry); err != nil {
		t.Fatal(err)
	}
	return entry
}

func TestFinishRefusesAClockThatSteppedBackAndWritesNoRecord(t *testing.T) {
	s, entry := frozenRun(t)
	future := time.Now().UTC().Add(time.Hour)
	entry = refrozen(t, s, entry, future)
	result := executor.Result{TaskName: "format-check", StartedAt: future, EndedAt: future}
	if _, err := s.Finish(entry, result, nil); err == nil || !errors.Is(err, errStampsOutOfOrder) {
		t.Fatalf("Finish accepted a record that finished before it started: %v", err)
	}
	if finishedRecordExists(t, s, entry.ID) {
		t.Fatal("a terminal record no server can ingest was written anyway")
	}
}

func TestFinishRefusesExecutionOutsideTheRecordsEnvelope(t *testing.T) {
	s, entry := frozenRun(t)
	result := executor.Result{TaskName: "format-check", StartedAt: entry.StartedAt,
		EndedAt: time.Now().UTC().Add(time.Hour)}
	if _, err := s.Finish(entry, result, nil); err == nil || !errors.Is(err, errStampsOutOfOrder) {
		t.Fatalf("Finish accepted execution ending after the record: %v", err)
	}
	if finishedRecordExists(t, s, entry.ID) {
		t.Fatal("a terminal record no server can ingest was written anyway")
	}
}

func TestFinishStillAcceptsAnOrdinaryRun(t *testing.T) {
	s, entry := frozenRun(t)
	result := executor.Result{TaskName: "format-check", StartedAt: entry.StartedAt,
		EndedAt: time.Now().UTC()}
	finished, err := s.Finish(entry, result, nil)
	if err != nil {
		t.Fatalf("refused an ordinary run: %v", err)
	}
	if finished.SyncState != "unsynced" || finished.FinishedAt == nil {
		t.Fatalf("bad finish: %+v", finished)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 1 {
		t.Fatalf("pending %d: %v", len(pending), err)
	}
}
