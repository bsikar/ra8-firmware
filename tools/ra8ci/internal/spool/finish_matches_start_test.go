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

const (
	frozenDigest   = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
	frozenCommit   = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	frozenSnapshot = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
)

// frozenRun begins an unverified local attempt, the ordinary shape for a run
// started from a dirty working tree.
func frozenRun(t *testing.T) (*Spool, Entry) {
	t.Helper()
	s, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatal(err)
	}
	entry, err := s.BeginWithMetadata("format-check", frozenDigest, Metadata{
		Source: SourceIdentity{Repository: "bsikar/ra8-firmware", Branch: "feature",
			CommitSHA: frozenCommit, Verification: "unverified"},
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 900,
		Args: []string{"--check"}})
	if err != nil {
		t.Fatal(err)
	}
	return s, entry
}

func finishedRecordExists(t *testing.T, s *Spool, id string) bool {
	t.Helper()
	_, err := os.Stat(filepath.Join(s.directory, id+".finished.json"))
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		t.Fatal(err)
	}
	return err == nil
}

func refuses(t *testing.T, s *Spool, entry Entry, want string) {
	t.Helper()
	_, err := s.Finish(entry, executor.Result{TaskName: entry.Task}, nil)
	if err == nil {
		t.Fatalf("terminal record accepted after %s changed", want)
	}
	if !errors.Is(err, errFinishDisagrees) || !strings.Contains(err.Error(), want) {
		t.Fatalf("refusal did not name %s: %v", want, err)
	}
	if finishedRecordExists(t, s, entry.ID) {
		t.Fatal("a refused terminal record was written anyway")
	}
}

func TestAnHonestFinishIsAccepted(t *testing.T) {
	s, entry := frozenRun(t)
	finished, err := s.Finish(entry, executor.Result{TaskName: "format-check", ExitCode: 0}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if finished.SyncState != "unsynced" || finished.FinishedAt == nil || finished.Result == nil {
		t.Fatalf("honest finish did not land: %+v", finished)
	}
}

// The reason this rule exists: a run that could prove nothing about its
// checkout must not finish claiming it could.
func TestSourceVerificationCannotBeUpgradedAfterExecution(t *testing.T) {
	s, entry := frozenRun(t)
	entry.Source.Verification = "verified"
	entry.Source.SnapshotSHA256 = frozenSnapshot
	refuses(t, s, entry, "source verification")
}

func TestASnapshotDigestAttachedAfterExecutionIsRefused(t *testing.T) {
	s, entry := frozenRun(t)
	entry.Source.SnapshotSHA256 = frozenSnapshot
	refuses(t, s, entry, "source snapshot digest")
}

func TestTheReviewedTaskAndCatalogCannotMoveBetweenTheTwoWrites(t *testing.T) {
	for _, testCase := range []struct {
		name   string
		change func(*Entry)
		field  string
	}{
		{"task", func(e *Entry) { e.Task = "unit-tests" }, "task"},
		{"catalog digest", func(e *Entry) { e.CatalogDigest = strings.Repeat("d", 64) }, "catalog digest"},
		{"tier", func(e *Entry) { e.Tier = "optional" }, "tier"},
		{"scope", func(e *Entry) { e.Scope = "safe-local-write" }, "scope"},
		{"deadline", func(e *Entry) { e.DeadlineSeconds = 60 }, "deadline"},
		{"repository", func(e *Entry) { e.Source.Repository = "someone/else" }, "source repository"},
		{"branch", func(e *Entry) { e.Source.Branch = "main" }, "source branch"},
		{"commit", func(e *Entry) { e.Source.CommitSHA = strings.Repeat("e", 40) }, "source commit"},
		{"schema version", func(e *Entry) { e.SchemaVersion = 1 }, "schema version"},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			s, entry := frozenRun(t)
			testCase.change(&entry)
			refuses(t, s, entry, testCase.field)
		})
	}
}

func TestArgumentsCannotChangeAfterExecution(t *testing.T) {
	s, entry := frozenRun(t)
	entry.Args = append(append([]string(nil), entry.Args...), "--fix")
	refuses(t, s, entry, "arguments")
}

// The start stamp is what the server subtracts the finish stamp from, so it
// decides the duration written to durable history.
func TestTheStartStampCannotBeMovedAfterExecution(t *testing.T) {
	s, entry := frozenRun(t)
	entry.StartedAt = entry.StartedAt.Add(-time.Hour)
	refuses(t, s, entry, "start stamp")
}

// Equality here is by instant, not by struct: the frozen record makes a round
// trip through JSON before it is compared.
func TestTheFrozenStampSurvivesItsRoundTripToDisk(t *testing.T) {
	s, entry := frozenRun(t)
	started, err := s.readStarted(entry.ID)
	if err != nil {
		t.Fatal(err)
	}
	if !started.StartedAt.Equal(entry.StartedAt) {
		t.Fatalf("stamp did not survive: %s vs %s", started.StartedAt, entry.StartedAt)
	}
	if err := checkFinishMatchesStart(started, entry); err != nil {
		t.Fatalf("a record compared against its own frozen copy was refused: %v", err)
	}
}

// What Finish is for still travels: the result and the run error are produced
// by the execution this record is reporting, so they are not frozen fields.
func TestTheResultAndRunErrorStillTravel(t *testing.T) {
	s, entry := frozenRun(t)
	finished, err := s.Finish(entry,
		executor.Result{TaskName: "format-check", ExitCode: 7, TimedOut: true},
		errors.New("step failed"))
	if err != nil {
		t.Fatal(err)
	}
	if finished.Result.ExitCode != 7 || !finished.Result.TimedOut || finished.Error != "step failed" {
		t.Fatalf("execution evidence was dropped: %+v", finished)
	}
}

func TestAMissingStartRecordIsStillRefused(t *testing.T) {
	s, entry := frozenRun(t)
	if err := os.Remove(filepath.Join(s.directory, entry.ID+".started.json")); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Finish(entry, executor.Result{}, nil); err == nil ||
		!strings.Contains(err.Error(), "missing start record") {
		t.Fatalf("missing start record accepted: %v", err)
	}
}
