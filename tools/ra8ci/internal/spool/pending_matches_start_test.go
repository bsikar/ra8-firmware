// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// spooledRun begins an unverified attempt and finishes it honestly, leaving a
// start record and a terminal record on disk, which is the state Pending reads.
func spooledRun(t *testing.T) (*Spool, Entry) {
	t.Helper()
	s, entry := frozenRun(t)
	finished, err := s.Finish(entry, executor.Result{TaskName: "format-check"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	return s, finished
}

// editTerminalRecord rewrites the terminal record on disk, which is what an
// edit after the run looks like: Finish is long gone and never sees it.
func editTerminalRecord(t *testing.T, s *Spool, entry Entry, change func(*Entry)) {
	t.Helper()
	change(&entry)
	raw, err := json.Marshal(entry)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(s.directory, entry.ID+".finished.json")
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0600); err != nil {
		t.Fatal(err)
	}
}

func TestAnHonestTerminalRecordIsStillPending(t *testing.T) {
	s, entry := spooledRun(t)
	pending, err := s.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 || pending[0].ID != entry.ID || pending[0].Result == nil {
		t.Fatalf("honest record not pending: %+v", pending)
	}
}

// The upload reads the file, not the caller's copy, so this is the door where
// an edit made after the run is caught.
func TestATerminalRecordEditedOnDiskIsRefused(t *testing.T) {
	for _, testCase := range []struct {
		name   string
		change func(*Entry)
		field  string
	}{
		{"verification upgraded", func(e *Entry) {
			e.Source.Verification = "verified"
			e.Source.SnapshotSHA256 = frozenSnapshot
		}, "source verification"},
		{"task renamed", func(e *Entry) { e.Task = "unit-tests" }, "task"},
		{"catalog digest swapped", func(e *Entry) { e.CatalogDigest = strings.Repeat("d", 64) }, "catalog digest"},
		{"scope widened", func(e *Entry) { e.Scope = "safe-local-write" }, "scope"},
		{"arguments added", func(e *Entry) { e.Args = append(append([]string(nil), e.Args...), "--fix") }, "arguments"},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			s, entry := spooledRun(t)
			editTerminalRecord(t, s, entry, testCase.change)
			pending, err := s.Pending()
			if err == nil {
				t.Fatalf("edited record offered for upload: %+v", pending)
			}
			if !errors.Is(err, errFinishDisagrees) || !strings.Contains(err.Error(), testCase.field) {
				t.Fatalf("refusal did not name %s: %v", testCase.field, err)
			}
			if pending != nil {
				t.Fatalf("records returned beside the refusal: %+v", pending)
			}
		})
	}
}

// Everything the execution produced is still free to differ: those fields are
// not in the frozen record at all.
func TestAResultOnTheTerminalRecordIsNotJudgedAgainstTheStart(t *testing.T) {
	s, entry := spooledRun(t)
	editTerminalRecord(t, s, entry, func(e *Entry) {
		e.Result = &executor.Result{TaskName: "format-check", ExitCode: 7, TimedOut: true}
		e.Error = "step failed"
	})
	pending, err := s.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 || pending[0].Result.ExitCode != 7 || pending[0].Error != "step failed" {
		t.Fatalf("execution evidence was refused or lost: %+v %v", pending, err)
	}
}

func TestATerminalRecordWhoseStartIsGoneIsRefused(t *testing.T) {
	s, entry := spooledRun(t)
	if err := os.Remove(filepath.Join(s.directory, entry.ID+".started.json")); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Pending(); err == nil || !strings.Contains(err.Error(), "missing start record") {
		t.Fatalf("record with no frozen start offered for upload: %v", err)
	}
}

// A synced record is skipped before its start record is ever read, so retiring
// a record does not depend on the pair still being intact.
func TestASyncedRecordIsSkippedBeforeTheStartIsRead(t *testing.T) {
	s, entry := spooledRun(t)
	if err := s.MarkSynced(entry.ID, "server-run-1"); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(filepath.Join(s.directory, entry.ID+".started.json")); err != nil {
		t.Fatal(err)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 0 {
		t.Fatalf("synced record was re-read: %+v %v", pending, err)
	}
}
