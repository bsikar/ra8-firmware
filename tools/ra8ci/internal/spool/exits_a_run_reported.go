// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// noReportedChildExit is the exit code the executor carries for work no child
// decided: what a Result and a StepResult are built with before anything has
// run, and what a step the context outlived keeps (executor.go). It stays
// acceptable here, because a record has no optional field to leave a code out
// of and the executor writes the sentinel to say a child never exited.
const noReportedChildExit = -1

// widestReportableExit is the largest number a supported runner can hand back.
// Not 255: Linux reports the byte the kernel puts in the wait status, but
// Windows reads the whole DWORD out of GetExitCodeProcess, which is where
// 0xC0000005 comes from, and a record is frozen without knowing which runner
// wrote it. Typed int64 because an untyped 1<<32-1 does not fit an int on a
// 32-bit build, and this CLI is built for the machines developers run it on.
const widestReportableExit = int64(1)<<32 - 1

// errUnreportableExit names the one thing this rule refuses: a result handed
// to Finish stating an exit code no runner could have read out of a child.
var errUnreportableExit = errors.New("execution result states an exit no runner could report")

// checkTheExitsWereReported holds the attempt's exit code and every step's at
// the freeze.
//
// A result states exit codes twice over, once for the attempt and once per
// step, and Finish took the executor's result by value, stored its address and
// wrote it out having looked at neither. They are the numbers durable history
// answers "did this pass" from: server.offlineInput copies the attempt's into
// store.LocalRunInput.ChildExitCode and each step's into
// store.LocalStepInput.ExitCode, and neither column carries a CHECK
// (0005_offline_sync.sql), so the bound is the only thing between a record and
// a row stating 70000.
//
// THE RULE, and it is the store's rather than a new one
// (store.exitCodesNameAChildThatRan): at or above the sentinel for a child
// that never exited, at or below the widest code a supported runner can read.
//
// This is the FREEZE, not the sweep, and the two doors are not the same door.
// checkUploadedExitsAreOnesARunnerCouldReport judges a record read back OFF
// DISK, where an older build's file or an edited one can say anything at all;
// this one judges what the executor in this process just handed over, before
// the bytes are written, so the outbox never holds the claim in the first
// place. Refused here the operator is told which step and which code while the
// run is still in hand; refused at the sweep the record sits in the outbox and
// every unsynced record behind it waits on every pass. A result this build
// produced cannot be refused by either: both runners open at the sentinel and
// write what they read from the child.
//
// DELIBERATELY NOT the agreement between a code and the rest of the record: a
// zero exit beside a cancelled step, or the sentinel beside a succeeded run,
// is a question about what the attempt MEANS, and the plane answers it where
// the whole record is in hand (store.validateLocalRun refuses a succeeded run
// whose code is not zero, and the migration states the same CHECK). This door
// asks only whether a child could have reported the number.
func checkTheExitsWereReported(result executor.Result) error {
	if err := reportableExit(result.ExitCode, "the result"); err != nil {
		return err
	}
	for i, step := range result.Steps {
		if err := reportableExit(step.ExitCode, namedStepOf(result, i)); err != nil {
			return err
		}
	}
	return nil
}

func reportableExit(code int, subject string) error {
	if code < noReportedChildExit {
		return fmt.Errorf("%w: %s states exit %d, below the code for a child that never exited",
			errUnreportableExit, subject, code)
	}
	if int64(code) > widestReportableExit {
		return fmt.Errorf("%w: %s states exit %d, wider than a runner can read from a child",
			errUnreportableExit, subject, code)
	}
	return nil
}
