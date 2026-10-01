// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

// noLocalChildExit is the exit code the executor carries for work no child
// decided: it is what Result and StepResult are built with before any child
// has run (executor.go), and what both runners open with (process_linux.go,
// process_windows.go). Nothing exited, so there is no code to report, and -1
// is how the executor says that.
const noLocalChildExit = -1

// widestLocallyReportedExit is the largest number a supported runner can hand
// back. Not 255: Linux reports the byte the kernel puts in the wait status, so
// a child there exits between 0 and 255, but Windows reads the process exit
// with GetExitCodeProcess and states the whole DWORD, which is where
// 0xC0000005 and 0xC000013A come from. A local run is read without knowing
// which runner produced it, so the bound is the wider of the two.
//
// Typed int64 rather than an untyped constant: an untyped 1<<32-1 does not fit
// an int on a 32-bit build, and the CLI that produces these records is built
// for the machines developers run it on, not only for amd64.
const widestLocallyReportedExit = int64(1)<<32 - 1

// exitCodesNameAChildThatRan holds every exit code a local run states to a
// number a child could have reported.
//
// Every other door onto these two columns already states this rule. The agent
// path holds a terminal receipt's attempt-level and per-step codes to the same
// two bounds (protocol.checkExitCodesNameAChildThatRan); the board path
// refuses a terminal child exit outside the range a child can report
// (board_hil_completion.go, 0..255 against a *int); and the offline HTTP door
// checks a spooled record before it builds this input
// (server.checkLocalExitCodesNameAChildThatRan). validateLocalRun, which is
// the store's own gate on the same two columns, bounds a step's ordinal, key,
// stamps, duration, digests and byte counts and says nothing about its exit
// code, nor about the run's.
//
// That the HTTP door checks it is not a reason to leave this one silent.
// IngestLocalRun is exported and takes a LocalRunInput, not a spool entry: any
// caller that builds the struct itself reaches these columns without passing
// the offline door at all, and validateLocalRun is what stands between such a
// caller and durable history. The same argument landed namesATextColumnCanHold
// here after the server-side rule, and it holds for a number as much as for a
// name: a door that admits what its own far end cannot interpret is not a
// door.
//
// The column itself will not catch it. local_runs.child_exit_code is a plain
// integer and local_run_steps.child_exit_code is integer NOT NULL, neither
// with a CHECK (0005_offline_sync.sql), so anything an int holds commits. A
// row stating 70000 looks like evidence and answers nothing, and it is read
// later to compare a local run against the agent-path attempt of the same
// task, a comparison between a bounded number and an unbounded one for as
// long as only one side states the rule.
//
// The sentinel itself stays acceptable, unlike on the agent path where an
// attempt no child decided states no code at all. ChildExitCode here is a
// plain int with no way to say nothing, and the executor writes -1 into it for
// a task that ended between steps. A run reporting the sentinel is already
// held elsewhere: validateLocalRun refuses a succeeded run whose code is not
// zero, and the migration states the same CHECK, so the sentinel can only
// arrive beside a non-success result, which is the honest report it is.
func exitCodesNameAChildThatRan(in LocalRunInput) bool {
	if !exitCodeNamesAChild(in.ChildExitCode) {
		return false
	}
	for _, step := range in.Steps {
		if !exitCodeNamesAChild(step.ExitCode) {
			return false
		}
	}
	return true
}

func exitCodeNamesAChild(code int) bool {
	return code >= noLocalChildExit && int64(code) <= widestLocallyReportedExit
}
