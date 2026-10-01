// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// noChildExitReported is the exit code the executor carries for work no child
// decided. It is the value a StepResult is built with and the value runCommand
// returns when the context was already spent before the child started, on every
// supported runner (executor.go, the StepResult and Result initializers;
// process_linux.go and process_windows.go both opening with ExitCode -1;
// between_steps.go, noChildExit). Nothing exited, so there is no code to
// report, and -1 is how the executor says that.
const noChildExitReported = -1

// widestReportedExit is the largest number a supported runner can hand back.
// It is deliberately not 255. Linux reports the byte the kernel puts in the
// wait status, so a child there exits between 0 and 255, but Windows reads the
// process exit with GetExitCodeProcess and states the whole DWORD
// (process_windows.go, result.ExitCode = int(exitCode) from a uint32), which is
// where the familiar 0xC0000005 access-violation and 0xC000013A Ctrl-C codes
// come from. A receipt is judged here without knowing which runner produced it,
// so the bound is the wider of the two: anything above it is a number no
// supported runner could have read out of a child at all.
//
// Typed int64 rather than an untyped constant on purpose: an untyped 1<<32-1
// does not fit an int on a 32-bit build, and the agent is built for the arches
// its runners have, not only for amd64.
const widestReportedExit = int64(1)<<32 - 1

// checkExitCodesNameAChildThatRan holds every exit code a terminal receipt
// states to a number a child could have reported. The receipt carries the codes
// in two places, and nothing judged either of them for range. Validate's
// per-step loop bounds a step's name, its stamps, its duration and its byte
// counts and says nothing about ExitCode, and the outcome switch judges
// ChildExitCode only under "succeeded", where it must be 0, so every other
// outcome carried whatever number the JSON held.
//
// Both fields have a narrow set of values the tree can produce. The agent
// states the attempt-level code from result.ExitCode and only when it is not
// negative (agent.go, terminalReceipt), precisely because a negative code is
// the executor's way of saying no child decided this attempt: a task that ended
// between two steps drops back to noChildExit rather than keeping the previous
// step's clean 0 (executor/between_steps.go), and the agent then states no code
// at all. So a receipt carrying a negative ChildExitCode says both things at
// once, that a child decided the attempt and that none did, and an absent field
// is the shape that says the second one honestly. A step's code is either that
// same sentinel or a code a child really returned, so below the sentinel is not
// a value any executor path writes.
//
// The numbers are read, not filed and forgotten. The plane writes the
// attempt-level code into task_attempts.child_exit_code and each step's into
// task_steps.child_exit_code (store/dispatch.go), the offline path writes the
// same numbers into local_runs and local_run_steps (store/local_sync.go), and
// those columns are what a later reader quotes for "what did it exit with" and
// what a retry policy reads to decide whether to run the work again. The board
// path already refuses a terminal child exit outside the range a child can
// report (store/board_hil_completion.go), so the agent path states the rule
// the board path already states rather than leaving one door narrower than the
// other.
//
// It runs before every rule that COMPARES exit codes, so that
// checkStepOutcomesAgreeWithTheAttempt and checkChildExitIsTheLastStepsExit are
// comparing two numbers that are each an exit code to begin with. A receipt
// whose last step reports the sentinel beside an attempt-level code is refused
// here, on the attempt's own field, rather than downstream as a mismatch
// between the two: the mismatch is true but it is not what is wrong.
func checkExitCodesNameAChildThatRan(receipt TerminalReceipt) error {
	if receipt.ChildExitCode != nil {
		code := *receipt.ChildExitCode
		if code < 0 {
			return fmt.Errorf("%w: attempt states child exit %d, the code for an attempt no child decided", ErrInvalid, code)
		}
		if int64(code) > widestReportedExit {
			return fmt.Errorf("%w: attempt states child exit %d, wider than a runner can read from a child", ErrInvalid, code)
		}
	}
	for _, step := range receipt.Steps {
		if step.ExitCode < noChildExitReported {
			return fmt.Errorf("%w: step %s states exit %d, below the code for a child that never exited", ErrInvalid, step.Name, step.ExitCode)
		}
		if int64(step.ExitCode) > widestReportedExit {
			return fmt.Errorf("%w: step %s states exit %d, wider than a runner can read from a child", ErrInvalid, step.Name, step.ExitCode)
		}
	}
	return nil
}
