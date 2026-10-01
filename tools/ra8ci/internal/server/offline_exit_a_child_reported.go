package server

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// noLocalChildExit is the exit code the executor carries for work no child
// decided. It is the value a Result and a StepResult are built with, before
// any child has run and for a step the context outlived (executor.go, the
// `Result{TaskName: task.Name, ExitCode: -1}` and
// `StepResult{Name: step.Name, ..., ExitCode: -1}` initializers), and it is
// what both runners open with (process_linux.go and process_windows.go).
// Nothing exited, so there is no code to report, and -1 is how the executor
// says that.
const noLocalChildExit = -1

// widestLocallyReportedExit is the largest number a supported runner can hand
// back. Not 255: Linux reports the byte the kernel puts in the wait status, so
// a child there exits between 0 and 255, but Windows reads the process exit
// with GetExitCodeProcess and states the whole DWORD, which is where
// 0xC0000005 and 0xC000013A come from. A spooled record is read without
// knowing which runner produced it, so the bound is the wider of the two.
//
// Typed int64 rather than an untyped constant: an untyped 1<<32-1 does not fit
// an int on a 32-bit build, and the CLI that spools these records is built for
// the machines developers run it on, not only for amd64.
const widestLocallyReportedExit = int64(1)<<32 - 1

// checkLocalExitCodesNameAChildThatRan holds every exit code a spooled record
// states to a number a child could have reported.
//
// The agent path already states this rule: a terminal receipt's attempt-level
// and per-step codes are held to the same two bounds before the plane will
// look at them (protocol.checkExitCodesNameAChildThatRan), and the board path
// refuses a terminal child exit outside the range a child can report
// (store/board_hil_completion.go). The offline path, which writes into the
// same two columns for the same reason, judged neither number at all.
// offlineInput reads entry.Result.ExitCode only to ask whether it is zero and
// copies it into LocalRunInput.ChildExitCode; it copies each step's code into
// LocalStepInput.ExitCode untouched; and validateLocalRun, the store's own
// gate, bounds the stamps, the digests and the byte counts of a step and says
// nothing about its exit code (store/local_sync.go). So a record stating
// math.MinInt64, or 70000, or any other number no process ever returned,
// landed in local_runs.child_exit_code and local_run_steps.child_exit_code as
// durable history.
//
// Those columns are read, not filed and forgotten. They are what a later
// reader quotes for "what did it exit with" and what compares a local run
// against the agent-path attempt of the same task, and that comparison is
// between a bounded number and an unbounded one as long as only one door
// states the rule. An uninterpretable code is worse than a missing one: a row
// holding 70000 looks like evidence and answers nothing.
//
// Both ends of the range matter and they say different things. Below the
// sentinel is a number no executor path writes, since -1 is already how the
// executor says no child decided this work. Above the widest DWORD is a number
// no runner could have read out of a child. The sentinel ITSELF stays
// acceptable here, unlike on the agent path where an attempt no child decided
// states no code at all: a spooled record has no optional field to leave out,
// entry.Result.ExitCode is a plain int, and the executor writes -1 into it for
// a task that ended between steps. offlineInput already maps that record to a
// non-success result, so the sentinel arrives as the honest report it is.
func checkLocalExitCodesNameAChildThatRan(entry spool.Entry) error {
	if entry.Result == nil {
		return fmt.Errorf("%w: the spooled record states no result", store.ErrInvalid)
	}
	if err := exitCodeNamesAChild(entry.Result.ExitCode, "the record"); err != nil {
		return err
	}
	for _, step := range entry.Result.Steps {
		if err := exitCodeNamesAChild(step.ExitCode, fmt.Sprintf("step %q", step.Name)); err != nil {
			return err
		}
	}
	return nil
}

func exitCodeNamesAChild(code int, subject string) error {
	if code < noLocalChildExit {
		return fmt.Errorf("%w: %s states exit %d, below the code for a child that never exited",
			store.ErrInvalid, subject, code)
	}
	if int64(code) > widestLocallyReportedExit {
		return fmt.Errorf("%w: %s states exit %d, wider than a runner can read from a child",
			store.ErrInvalid, subject, code)
	}
	return nil
}
