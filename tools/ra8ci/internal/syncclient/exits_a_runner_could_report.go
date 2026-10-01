// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// noReportedChildExit is the exit code the executor carries for work no child
// decided: the value a Result and a StepResult are built with before any child
// has run, and what a step the context outlived keeps (executor.go). It stays
// acceptable here for the same reason the server accepts it, since a spooled
// record has no optional field to leave a code out of.
const noReportedChildExit = -1

// widestReportableExit is the largest number a supported runner can hand back.
// Not 255: Linux reports the byte the kernel puts in the wait status, but
// Windows reads the whole DWORD out of GetExitCodeProcess, which is where
// 0xC0000005 comes from, and a record is uploaded without knowing which runner
// wrote it. Typed int64 because an untyped 1<<32-1 does not fit an int on a
// 32-bit build, and this CLI is built for the machines developers run it on.
const widestReportableExit = int64(1)<<32 - 1

// ErrUnreportableExit is the refusal of a local record stating an exit code no
// runner could have read out of a child.
var ErrUnreportableExit = errors.New("local record states an exit no runner could report")

// The record states exit codes twice over: once for the attempt and once per
// step, and this client posted both unexamined. Everything it does ask about a
// record is about identity, stamps, names and size, so a record carrying
// math.MinInt64 or 70000 passed every door here and left the host.
//
// The far end refuses it. server.checkLocalExitCodesNameAChildThatRan holds
// the attempt's code and every step's to these same two bounds before
// offlineInput copies them into LocalRunInput.ChildExitCode and each
// LocalStepInput.ExitCode, and the store restates the rule again at
// validateLocalRun (exitCodesNameAChildThatRan) before the insert. So nothing
// uninterpretable was reaching local_runs.child_exit_code.
//
// *** HONESTY: this refusal changes nothing about what the database holds. It
// buys what the three record-only doors above it buy, and for the same reason:
// where the sweep stops and what it says when it does. Refused there, the
// client reads back "upload local <id> returned HTTP 400", which is also what
// a server that is merely unwell says, and SyncPending turns any non-200 into
// a returned error that ends the whole sweep. Pending hands a record back
// until a synced marker sits beside it, so the same record is read again,
// posted again and refused again on every pass, and every unsynced record
// behind it in the outbox waits behind it on every pass too. Refused here, the
// operator is told which record and which step and which code, before the
// bytes leave the host that wrote them.
//
// Judged on the record alone, needing neither the catalog nor the bytes, which
// is what lets it sit with the other three rather than after the marshal.
func checkUploadedExitsAreOnesARunnerCouldReport(entry spool.Entry) error {
	if entry.Result == nil {
		return nil
	}
	if err := reportableExit(entry.Result.ExitCode, "the record"); err != nil {
		return err
	}
	for _, step := range entry.Result.Steps {
		if err := reportableExit(step.ExitCode, fmt.Sprintf("step %q", step.Name)); err != nil {
			return err
		}
	}
	return nil
}

func reportableExit(code int, subject string) error {
	if code < noReportedChildExit {
		return fmt.Errorf("%w: %s states exit %d, below the code for a child that never exited",
			ErrUnreportableExit, subject, code)
	}
	if int64(code) > widestReportableExit {
		return fmt.Errorf("%w: %s states exit %d, wider than a runner can read from a child",
			ErrUnreportableExit, subject, code)
	}
	return nil
}
