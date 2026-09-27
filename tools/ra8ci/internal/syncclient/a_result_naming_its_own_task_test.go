package syncclient

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// namingTask builds a terminal record for task, whose result names ran.
func namingTask(task, ran string) spool.Entry {
	started := time.Date(2026, 9, 27, 13, 0, 0, 0, time.UTC)
	finished := started.Add(2 * time.Minute)
	return spool.Entry{
		SchemaVersion: 2,
		ID:            strings.Repeat("c", 32),
		Task:          task,
		StartedAt:     started,
		FinishedAt:    &finished,
		SyncState:     "unsynced",
		Result: &executor.Result{
			TaskName:  ran,
			StartedAt: started,
			EndedAt:   finished,
		},
	}
}

func TestAResultNamingItsOwnTaskIsUploaded(t *testing.T) {
	if err := checkUploadedResultNamesItsTask(namingTask("format-check", "format-check")); err != nil {
		t.Fatalf("a result naming the record's own task was refused: %v", err)
	}
}

func TestAResultNamingAnotherTaskIsRefused(t *testing.T) {
	err := checkUploadedResultNamesItsTask(namingTask("format-check", "unit-tests"))
	if !errors.Is(err, ErrResultNamesAnotherTask) {
		t.Fatalf("a result naming another task was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "format-check") || !strings.Contains(err.Error(), "unit-tests") {
		t.Fatalf("the refusal names neither the record's task nor the result's: %v", err)
	}
}

// The two names are held exactly, not folded: durable history keys the run on
// the record's task and files the result under it, and a catalog task name is
// one string, never two spellings of one.
func TestAResultNamingTheTaskInAnotherCaseIsRefused(t *testing.T) {
	if err := checkUploadedResultNamesItsTask(namingTask("format-check", "Format-Check")); !errors.Is(err, ErrResultNamesAnotherTask) {
		t.Fatalf("a result naming the task in another case was not refused: %v", err)
	}
}

func TestAResultNamingTheTaskWithSurroundingSpaceIsRefused(t *testing.T) {
	if err := checkUploadedResultNamesItsTask(namingTask("format-check", " format-check")); !errors.Is(err, ErrResultNamesAnotherTask) {
		t.Fatalf("a result naming the task with leading space was not refused: %v", err)
	}
}

// An attempt that came apart before the executor filled its result in claims
// nothing about what ran, and the server takes it, so this door does too.
func TestAResultNamingNoTaskIsUploaded(t *testing.T) {
	if err := checkUploadedResultNamesItsTask(namingTask("format-check", "")); err != nil {
		t.Fatalf("a result stating no task name was refused: %v", err)
	}
}

// A record with no result at all is the spool's refusal, made at its own door
// (checkTerminalRecordCarriesItsRun), and this rule does not restate it: a nil
// result here is passed on untouched rather than given a second name.
func TestARecordCarryingNoResultIsNotJudgedHere(t *testing.T) {
	entry := namingTask("format-check", "format-check")
	entry.Result = nil
	if err := checkUploadedResultNamesItsTask(entry); err != nil {
		t.Fatalf("a record carrying no result was refused by the task-name door: %v", err)
	}
}

// A record stating no task of its own is still a disagreement when the result
// names one: the empty side is the record's, not the result's.
func TestARecordStatingNoTaskBesideANamedResultIsRefused(t *testing.T) {
	if err := checkUploadedResultNamesItsTask(namingTask("", "format-check")); !errors.Is(err, ErrResultNamesAnotherTask) {
		t.Fatalf("a record stating no task beside a named result was not refused: %v", err)
	}
}

func TestNeitherSideNamingATaskIsUploaded(t *testing.T) {
	if err := checkUploadedResultNamesItsTask(namingTask("", "")); err != nil {
		t.Fatalf("a record naming no task beside a result naming none was refused: %v", err)
	}
}
