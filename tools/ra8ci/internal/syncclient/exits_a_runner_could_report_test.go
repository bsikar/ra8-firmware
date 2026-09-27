package syncclient

import (
	"errors"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// exiting builds a terminal record whose attempt exits with attempt and whose
// one step exits with step.
func exiting(attempt, step int) spool.Entry {
	started := time.Date(2026, 9, 27, 14, 0, 0, 0, time.UTC)
	finished := started.Add(90 * time.Second)
	return spool.Entry{
		SchemaVersion: 2,
		ID:            strings.Repeat("d", 32),
		Task:          "unit-tests",
		StartedAt:     started,
		FinishedAt:    &finished,
		SyncState:     "unsynced",
		Result: &executor.Result{
			TaskName:  "unit-tests",
			StartedAt: started,
			EndedAt:   finished,
			ExitCode:  attempt,
			Steps: []executor.StepResult{{
				Name: "go test", StartedAt: started, EndedAt: finished, ExitCode: step,
			}},
		},
	}
}

func TestACodeAChildCouldHaveReportedIsUploaded(t *testing.T) {
	for _, code := range []int{0, 1, 2, 125, 255, 256, 3221225477} {
		if err := checkUploadedExitsAreOnesARunnerCouldReport(exiting(code, code)); err != nil {
			t.Fatalf("exit %d was refused: %v", code, err)
		}
	}
}

// The sentinel is how the executor says no child decided this work, and a
// spooled record has no optional field to leave a code out of, so it is the
// honest report rather than a shape to refuse.
func TestTheSentinelForAChildThatNeverExitedIsUploaded(t *testing.T) {
	if err := checkUploadedExitsAreOnesARunnerCouldReport(exiting(noReportedChildExit, noReportedChildExit)); err != nil {
		t.Fatalf("the no-child sentinel was refused: %v", err)
	}
}

func TestACodeBelowTheSentinelIsRefused(t *testing.T) {
	err := checkUploadedExitsAreOnesARunnerCouldReport(exiting(-2, 0))
	if !errors.Is(err, ErrUnreportableExit) {
		t.Fatalf("an exit below the sentinel was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "the record") || !strings.Contains(err.Error(), "-2") {
		t.Fatalf("the refusal names neither the subject nor the code: %v", err)
	}
}

func TestTheWidestCodeAWindowsRunnerReadsIsUploaded(t *testing.T) {
	if int64(math.MaxInt) < widestReportableExit {
		t.Skip("an int on this build cannot hold the widest DWORD")
	}
	if err := checkUploadedExitsAreOnesARunnerCouldReport(exiting(int(widestReportableExit), 0)); err != nil {
		t.Fatalf("the widest reportable exit was refused: %v", err)
	}
}

func TestACodeWiderThanARunnerReadsIsRefused(t *testing.T) {
	if int64(math.MaxInt) <= widestReportableExit {
		t.Skip("an int on this build cannot hold a code wider than the DWORD")
	}
	if err := checkUploadedExitsAreOnesARunnerCouldReport(exiting(int(widestReportableExit+1), 0)); !errors.Is(err, ErrUnreportableExit) {
		t.Fatalf("an exit wider than a runner reads was not refused: %v", err)
	}
}

// Every step is read, not only the first, and the refusal names which one.
func TestAStepStatingAnUnreportableCodeIsRefused(t *testing.T) {
	entry := exiting(0, 0)
	entry.Result.Steps = append(entry.Result.Steps, executor.StepResult{
		Name: "go vet", StartedAt: entry.StartedAt, EndedAt: *entry.FinishedAt, ExitCode: math.MinInt,
	})
	err := checkUploadedExitsAreOnesARunnerCouldReport(entry)
	if !errors.Is(err, ErrUnreportableExit) {
		t.Fatalf("a step stating an unreportable code was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "go vet") {
		t.Fatalf("the refusal does not name the step: %v", err)
	}
}

// A record with no result at all is spool's own refusal (it holds a terminal
// record to carrying its run), and the server states the same thing again at
// offlineInput. This door says nothing about it rather than growing a second
// opinion on a shape it was not written to judge.
func TestARecordWithNoResultIsLeftToTheDoorsThatJudgeIt(t *testing.T) {
	entry := exiting(0, 0)
	entry.Result = nil
	if err := checkUploadedExitsAreOnesARunnerCouldReport(entry); err != nil {
		t.Fatalf("a record with no result was refused here: %v", err)
	}
}

// The bound is the server's own, not a second opinion, so the two numbers are
// pinned together: a record the server reads is one this client will send.
func TestTheBoundsAreTheOnesTheServerReads(t *testing.T) {
	if noReportedChildExit != -1 {
		t.Fatalf("the no-child sentinel drifted from the executor's: %d", noReportedChildExit)
	}
	if widestReportableExit != int64(1)<<32-1 {
		t.Fatalf("the widest reportable exit drifted from the runner's: %d", widestReportableExit)
	}
}
