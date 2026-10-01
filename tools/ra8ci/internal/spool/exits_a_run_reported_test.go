// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"math"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

func exited(runExit int, stepExits ...int) executor.Result {
	steps := make([]executor.StepResult, 0, len(stepExits))
	for i, code := range stepExits {
		step := measuredStep("step")
		step.Name = []string{"build", "test", "package"}[i%3]
		step.ExitCode = code
		steps = append(steps, step)
	}
	result := resultOf(steps...)
	result.ExitCode = runExit
	return result
}

func TestARunThatPassedIsFrozen(t *testing.T) {
	if err := checkTheExitsWereReported(exited(0, 0, 0)); err != nil {
		t.Fatalf("a passing run was refused: %v", err)
	}
}

func TestARunThatFailedIsFrozen(t *testing.T) {
	// A nonzero child exit is the ordinary report of a failing task, not a
	// malformed record.
	if err := checkTheExitsWereReported(exited(1, 0, 1)); err != nil {
		t.Fatalf("a failing run was refused: %v", err)
	}
}

func TestTheSentinelForAChildThatNeverExitedIsFrozen(t *testing.T) {
	// The executor writes -1 for work no child decided, which is how a task
	// that came apart between steps says so.
	if err := checkTheExitsWereReported(exited(noReportedChildExit, noReportedChildExit)); err != nil {
		t.Fatalf("the sentinel was refused: %v", err)
	}
}

func TestAWindowsStatusIsFrozen(t *testing.T) {
	// 0xC0000005 is what GetExitCodeProcess reports for an access violation,
	// and it is far past the byte Linux reports.
	if err := checkTheExitsWereReported(exited(0xC0000005, 0xC000013A)); err != nil {
		t.Fatalf("a Windows status was refused: %v", err)
	}
}

func TestTheWidestReportableExitIsFrozen(t *testing.T) {
	if err := checkTheExitsWereReported(exited(int(widestReportableExit))); err != nil {
		t.Fatalf("the widest reportable exit was refused: %v", err)
	}
}

func TestAResultWithNoStepsIsFrozen(t *testing.T) {
	if err := checkTheExitsWereReported(exited(0)); err != nil {
		t.Fatalf("a result carrying no steps was refused: %v", err)
	}
}

func TestAnAttemptExitBelowTheSentinelIsRefused(t *testing.T) {
	if err := checkTheExitsWereReported(exited(-2, 0)); !errors.Is(err, errUnreportableExit) {
		t.Fatalf("an attempt stating exit -2 was frozen: %v", err)
	}
}

func TestAStepExitBelowTheSentinelIsRefused(t *testing.T) {
	if err := checkTheExitsWereReported(exited(0, 0, math.MinInt32)); !errors.Is(err, errUnreportableExit) {
		t.Fatalf("a step stating the smallest int32 was frozen: %v", err)
	}
}

func TestAnAttemptExitWiderThanARunnerCanReadIsRefused(t *testing.T) {
	if int64(math.MaxInt) <= widestReportableExit {
		t.Skip("int is too narrow on this build to state a wider exit")
	}
	if err := checkTheExitsWereReported(exited(int(widestReportableExit + 1))); !errors.Is(err, errUnreportableExit) {
		t.Fatalf("an attempt stating a wider exit was frozen: %v", err)
	}
}

func TestAStepExitWiderThanARunnerCanReadIsRefused(t *testing.T) {
	if int64(math.MaxInt) <= widestReportableExit {
		t.Skip("int is too narrow on this build to state a wider exit")
	}
	if err := checkTheExitsWereReported(exited(0, 0, int(widestReportableExit+1))); !errors.Is(err, errUnreportableExit) {
		t.Fatalf("a step stating a wider exit was frozen: %v", err)
	}
}

func TestTheRefusalNamesTheStepAndTheCode(t *testing.T) {
	err := checkTheExitsWereReported(exited(0, 0, -7))
	if err == nil {
		t.Fatal("a step stating exit -7 was frozen")
	}
	for _, want := range []string{"test", "-7"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal %q does not name %q", err, want)
		}
	}
}

func TestTheExitDoorDoesNotJudgeWhatTheCodeMeans(t *testing.T) {
	// A zero exit beside a cancelled step is a question about what the
	// attempt means, and the plane answers it where the whole record is in
	// hand. This door asks only whether a child could have reported the
	// number.
	result := exited(0, 0)
	result.Cancelled = true
	result.Steps[0].Cancelled = true
	if err := checkTheExitsWereReported(result); err != nil {
		t.Fatalf("a cancelled step reporting a zero exit was refused: %v", err)
	}
}
