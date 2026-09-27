// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"math"
	"testing"
)

// localRunReportingExit builds a valid non-success local run, so a case can
// state an exit code without contradicting the succeeded-means-zero rule that
// validateLocalRun and the migration both already hold.
func localRunReportingExit(runExit int, stepExit int) LocalRunInput {
	in := localRun()
	in.Result = "failed"
	in.ChildExitCode = runExit
	in.Steps[0].ExitCode = stepExit
	return in
}

func TestAnOrdinaryFailingExitIsAccepted(t *testing.T) {
	if err := validateLocalRun(localRunReportingExit(1, 1)); err != nil {
		t.Fatalf("a run reporting exit 1 was refused: %v", err)
	}
}

func TestTheSentinelForWorkNoChildDecidedIsAccepted(t *testing.T) {
	if err := validateLocalRun(localRunReportingExit(noLocalChildExit, noLocalChildExit)); err != nil {
		t.Fatalf("a run reporting the no-child sentinel was refused: %v", err)
	}
}

func TestAWindowsExceptionCodeIsAccepted(t *testing.T) {
	for _, code := range []int{0xC0000005, 0xC000013A} {
		if err := validateLocalRun(localRunReportingExit(code, code)); err != nil {
			t.Fatalf("a run reporting Windows exit %#x was refused: %v", code, err)
		}
	}
}

func TestTheWidestReportableExitIsAccepted(t *testing.T) {
	widest := int(widestLocallyReportedExit)
	if int64(widest) != widestLocallyReportedExit {
		t.Skip("this build's int cannot hold the widest DWORD")
	}
	if err := validateLocalRun(localRunReportingExit(widest, widest)); err != nil {
		t.Fatalf("a run reporting the widest DWORD was refused: %v", err)
	}
}

func TestARunExitBelowTheSentinelIsRefused(t *testing.T) {
	for _, code := range []int{-2, -256, math.MinInt32} {
		err := validateLocalRun(localRunReportingExit(code, 1))
		if err == nil {
			t.Fatalf("a run reporting exit %d was accepted", code)
		}
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("the refusal of exit %d does not travel as invalid: %v", code, err)
		}
	}
}

func TestAStepExitBelowTheSentinelIsRefused(t *testing.T) {
	if err := validateLocalRun(localRunReportingExit(1, -2)); err == nil {
		t.Fatal("a step reporting exit -2 was accepted")
	} else if !errors.Is(err, ErrInvalid) {
		t.Fatalf("the refusal does not travel as invalid: %v", err)
	}
}

func TestAnExitWiderThanARunnerCanReadIsRefused(t *testing.T) {
	wider := int(widestLocallyReportedExit) + 1
	if int64(wider) <= widestLocallyReportedExit {
		t.Skip("this build's int cannot hold a number above the widest DWORD")
	}
	if err := validateLocalRun(localRunReportingExit(wider, 1)); err == nil {
		t.Fatal("a run reporting an exit wider than a DWORD was accepted")
	} else if !errors.Is(err, ErrInvalid) {
		t.Fatalf("the refusal does not travel as invalid: %v", err)
	}
	if err := validateLocalRun(localRunReportingExit(1, wider)); err == nil {
		t.Fatal("a step reporting an exit wider than a DWORD was accepted")
	}
}

func TestTheRefusalIsAboutTheExitCodeNotTheRest(t *testing.T) {
	in := localRunReportingExit(math.MinInt32, 1)
	if err := validateLocalRun(in); err == nil {
		t.Fatal("the fixture did not exercise the exit rule")
	}
	in.ChildExitCode = 1
	if err := validateLocalRun(in); err != nil {
		t.Fatalf("the same run with an ordinary exit was refused: %v", err)
	}
}

func TestASucceededRunStillMayOnlyReportZero(t *testing.T) {
	in := localRunReportingExit(1, 0)
	in.Result = "succeeded"
	if err := validateLocalRun(in); err == nil {
		t.Fatal("a succeeded run reporting exit 1 was accepted")
	}
}

func TestEveryStepsExitIsJudgedNotOnlyTheFirsts(t *testing.T) {
	in := localRunReportingExit(1, 1)
	second := in.Steps[0]
	second.Key = "second-step"
	second.Ordinal = 1
	second.ExitCode = math.MinInt32
	in.Steps = append(in.Steps, second)
	if err := validateLocalRun(in); err == nil {
		t.Fatal("a second step reporting an impossible exit was accepted")
	} else if !errors.Is(err, ErrInvalid) {
		t.Fatalf("the refusal does not travel as invalid: %v", err)
	}
}
