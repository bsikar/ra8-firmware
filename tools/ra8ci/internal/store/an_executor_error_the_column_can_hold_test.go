// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
)

// localRunReporting builds a local run that failed and states why, so a case
// says only what it is judging about the message.
func localRunReporting(message string) LocalRunInput {
	run := localRun()
	run.Result = "incomplete_evidence"
	run.ChildExitCode = -1
	run.ExecutorError = message
	return run
}

func TestAStatedExecutorErrorIsAccepted(t *testing.T) {
	for _, message := range []string{
		"",
		"start child: exec format error",
		"run step \"format-tree-check\": signal: killed",
	} {
		if err := validateLocalRun(localRunReporting(message)); err != nil {
			t.Fatalf("executor error %q was refused: %v", message, err)
		}
	}
}

func TestAnExecutorErrorCarryingANULIsRefused(t *testing.T) {
	err := validateLocalRun(localRunReporting("start child\x00: exec format error"))
	if err == nil {
		t.Fatal("an executor error carrying a NUL was accepted")
	}
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("the refusal does not travel as invalid: %v", err)
	}
}

func TestAnExecutorErrorThatIsNotUTF8IsRefused(t *testing.T) {
	err := validateLocalRun(localRunReporting("step output: " + string([]byte{0xff, 0xfe, 0xfd})))
	if err == nil {
		t.Fatal("an executor error that is not UTF-8 was accepted")
	}
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("the refusal does not travel as invalid: %v", err)
	}
}

// A wrapped executor failure runs to several lines, and the tail of a child's
// output is a common thing to wrap. Those are messages a person reads, not
// identities a run is matched by, so the control range stays acceptable here
// where the task name and the step keys refuse it.
func TestAMultiLineExecutorErrorIsAccepted(t *testing.T) {
	for _, c := range []struct {
		name  string
		value string
	}{
		{"a newline", "run step: exit status 2\nformat-tree-check: 3 files differ"},
		{"a carriage return", "run step: exit status 2\r\nformat-tree-check failed"},
		{"a tab", "run step: exit status 2\n\tformat-tree-check failed"},
		{"a coloured child line", "run step: \x1b[31mFAIL\x1b[0m format-tree-check"},
	} {
		if err := validateLocalRun(localRunReporting(c.value)); err != nil {
			t.Fatalf("an executor error carrying %s was refused: %v", c.name, err)
		}
	}
}

func TestANonASCIIExecutorErrorIsAccepted(t *testing.T) {
	if err := validateLocalRun(localRunReporting("échec de la vérification du format")); err != nil {
		t.Fatalf("a non-ASCII executor error was refused: %v", err)
	}
}

// The length bound this rule sits beside is unchanged: it answers how much can
// arrive, and this one answers what the column can hold.
func TestTheExecutorErrorLengthBoundStillHolds(t *testing.T) {
	if err := validateLocalRun(localRunReporting(strings.Repeat("a", 1024))); err != nil {
		t.Fatalf("an executor error at the bound was refused: %v", err)
	}
	if err := validateLocalRun(localRunReporting(strings.Repeat("a", 1025))); err == nil {
		t.Fatal("an executor error past the bound was accepted")
	}
}

// A success may not state an executor error at all, so the refusal a malformed
// message earns must not depend on the contradiction rule catching it first.
func TestAMalformedExecutorErrorIsRefusedOnAFailedRun(t *testing.T) {
	run := localRunReporting("start child\x00")
	run.Result = "failed"
	run.ChildExitCode = 2
	if err := validateLocalRun(run); err == nil {
		t.Fatal("a failed run stating a malformed executor error was accepted")
	}
}

func TestExecutorErrorRuleReadsEachSpelling(t *testing.T) {
	for _, c := range []struct {
		name     string
		value    string
		holdable bool
	}{
		{"empty", "", true},
		{"plain ASCII", "exec format error", true},
		{"a newline", "one\ntwo", true},
		{"a delete", "one\x7ftwo", true},
		{"a C1 control", "one\u0085two", true},
		{"a NUL", "one\x00two", false},
		{"a lone continuation byte", string([]byte{0x80}), false},
		{"a truncated rune", "caf" + string([]byte{0xc3}), false},
	} {
		if got := executorErrorATextColumnCanHold(c.value); got != c.holdable {
			t.Fatalf("%s: holdable = %v, want %v", c.name, got, c.holdable)
		}
	}
}
