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

// Every freeze door has its own test beside it, holding what the check
// decides. These hold something else: that the refusal actually reaches the
// caller of Finish, and that a refused freeze writes NOTHING. A door that
// refuses correctly but lets the record land anyway leaves the outbox holding
// exactly the claim the door exists to keep out, and every unsynced record
// behind it waits on the sync that the bad one fails.

func aRunningSpool(t *testing.T) (*Spool, Entry) {
	t.Helper()
	s, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatal(err)
	}
	entry, err := s.Begin("ra8ci:build", strings.Repeat("a", 64))
	if err != nil {
		t.Fatal(err)
	}
	return s, entry
}

// measuredStep is a step as the executor hands one over: both streams
// measured, an exit a child could have reported, one ending.
func stepAsMeasured(name string) executor.StepResult {
	now := time.Now().UTC()
	return executor.StepResult{
		Name:         name,
		StartedAt:    now,
		EndedAt:      now,
		StdoutSHA256: strings.Repeat("b", 64),
		StderrSHA256: strings.Repeat("c", 64),
	}
}

func resultAsMeasured(entry Entry) executor.Result {
	now := time.Now().UTC()
	return executor.Result{
		TaskName:  entry.Task,
		StartedAt: now,
		EndedAt:   now,
		Steps:     []executor.StepResult{stepAsMeasured("compile")},
	}
}

func terminalRecords(t *testing.T, s *Spool) []string {
	t.Helper()
	entries, err := os.ReadDir(s.directory)
	if err != nil {
		t.Fatal(err)
	}
	var found []string
	for _, entry := range entries {
		if strings.HasSuffix(entry.Name(), ".finished.json") {
			found = append(found, entry.Name())
		}
	}
	return found
}

// The honest case first, so the refusals below are known to be about the
// result and not about the fixture.
func TestAMeasuredResultIsFrozen(t *testing.T) {
	s, entry := aRunningSpool(t)

	frozen, err := s.Finish(entry, resultAsMeasured(entry), nil)
	if err != nil {
		t.Fatalf("a result the executor could have produced was refused: %v", err)
	}
	if frozen.SyncState != "unsynced" {
		t.Fatalf("sync state = %q, want unsynced", frozen.SyncState)
	}
	if frozen.FinishedAt == nil || frozen.Result == nil {
		t.Fatal("the frozen record carries neither a finish stamp nor the result")
	}
	if records := terminalRecords(t, s); len(records) != 1 {
		t.Fatalf("terminal records = %v, want exactly one", records)
	}
}

// Each door, driven through Finish: the refusal names the step it is about,
// and the outbox is left with no terminal record at all.
func TestAResultTheDoorsRefuseNeverReachesTheOutbox(t *testing.T) {
	for _, refused := range []struct {
		door  string
		spoil func(*executor.Result)
		says  string
	}{
		{
			door:  "an unmeasured stream",
			spoil: func(r *executor.Result) { r.Steps[0].StdoutSHA256 = "not-a-digest" },
			says:  "digest",
		},
		{
			door:  "a negative byte count",
			spoil: func(r *executor.Result) { r.Steps[0].StderrBytes = -1 },
			says:  "bytes of stderr",
		},
		{
			door:  "an exit no child could report",
			spoil: func(r *executor.Result) { r.Steps[0].ExitCode = -2 },
			says:  "exit",
		},
		{
			door:  "an attempt exit no child could report",
			spoil: func(r *executor.Result) { r.ExitCode = 1 << 33 },
			says:  "exit",
		},
		{
			door:  "a step ending two ways",
			spoil: func(r *executor.Result) { r.Steps[0].TimedOut, r.Steps[0].Cancelled = true, true },
			says:  "both ran out of time and was called off",
		},
		{
			door:  "two steps sharing one name",
			spoil: func(r *executor.Result) { r.Steps = append(r.Steps, stepAsMeasured("compile")) },
			says:  "name",
		},
		{
			door:  "a result naming another task",
			spoil: func(r *executor.Result) { r.TaskName = "ra8ci:something-else" },
			says:  "task",
		},
	} {
		t.Run(refused.door, func(t *testing.T) {
			s, entry := aRunningSpool(t)
			result := resultAsMeasured(entry)
			refused.spoil(&result)

			_, err := s.Finish(entry, result, nil)
			if err == nil {
				t.Fatal("the freeze took a result the doors are there to refuse")
			}
			if !strings.Contains(err.Error(), refused.says) {
				t.Fatalf("error = %v, want it to name %q", err, refused.says)
			}
			if records := terminalRecords(t, s); len(records) != 0 {
				t.Fatalf("a refused freeze wrote %v", records)
			}
		})
	}
}

// The doors judge the result, but Finish judges the record it was handed
// first: an identifier this spool never issued, a record that is not running,
// and a start record that is gone are all refused before any door is asked.
func TestAFreezeJudgesTheRecordBeforeTheResult(t *testing.T) {
	t.Run("an identifier this spool never issued", func(t *testing.T) {
		s, entry := aRunningSpool(t)
		entry.ID = "nope"
		if _, err := s.Finish(entry, resultAsMeasured(entry), nil); err == nil ||
			!strings.Contains(err.Error(), "invalid running local record") {
			t.Fatalf("error = %v, want the identifier refused", err)
		}
	})

	t.Run("a record that is not running", func(t *testing.T) {
		s, entry := aRunningSpool(t)
		entry.SyncState = "unsynced"
		if _, err := s.Finish(entry, resultAsMeasured(entry), nil); err == nil ||
			!strings.Contains(err.Error(), "invalid running local record") {
			t.Fatalf("error = %v, want a record that is not running refused", err)
		}
	})

	t.Run("a start record that is gone", func(t *testing.T) {
		s, entry := aRunningSpool(t)
		if err := os.Remove(filepath.Join(s.directory, entry.ID+".started.json")); err != nil {
			t.Fatal(err)
		}
		if _, err := s.Finish(entry, resultAsMeasured(entry), nil); err == nil ||
			!strings.Contains(err.Error(), "missing start record") {
			t.Fatalf("error = %v, want the absent start record reported", err)
		}
		if records := terminalRecords(t, s); len(records) != 0 {
			t.Fatalf("a freeze with no start record wrote %v", records)
		}
	})
}

// A run that failed is still frozen: the error the executor wrapped is filed
// into its own column rather than refused, and the start record survives the
// freeze so the frozen half of the run is still readable afterwards.
func TestAFailedRunIsFrozenWithItsMessageAndKeepsItsStart(t *testing.T) {
	s, entry := aRunningSpool(t)

	frozen, err := s.Finish(entry, resultAsMeasured(entry), errors.New("compile step exited 2"))
	if err != nil {
		t.Fatal(err)
	}
	if frozen.Error != "compile step exited 2" {
		t.Fatalf("error column = %q, want the executor's message", frozen.Error)
	}
	started, err := s.readStarted(entry.ID)
	if err != nil {
		t.Fatalf("the freeze erased the start record: %v", err)
	}
	if started.Task != entry.Task {
		t.Fatalf("start record task = %q, want %q", started.Task, entry.Task)
	}
	pending, err := s.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 || pending[0].ID != entry.ID {
		t.Fatalf("pending = %+v, want the one unsynced record", pending)
	}
}
