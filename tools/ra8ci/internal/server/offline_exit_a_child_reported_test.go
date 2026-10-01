package server

import (
	"errors"
	"math"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// exited builds a record stating one attempt-level code and one step code, so
// a case reads as the pair it is judging.
func exited(t *testing.T, attempt, step int) spool.Entry {
	t.Helper()
	return stepped(t, func(e *spool.Entry) {
		e.Result.ExitCode = attempt
		e.Result.Steps[0].ExitCode = step
	})
}

func refused(t *testing.T, entry spool.Entry, what string) {
	t.Helper()
	_, cat := offlineTestEntry(t)
	in, err := offlineInput(entry, cat)
	if err == nil {
		t.Fatalf("%s was stored: exit %d, steps %+v", what, in.ChildExitCode, in.Steps)
	}
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("%s refusal does not travel as invalid: %v", what, err)
	}
}

func TestAReportedExitIsAccepted(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	in, err := offlineInput(entry, cat)
	if err != nil {
		t.Fatalf("a reported exit was refused: %v", err)
	}
	if in.ChildExitCode != 0 || in.Result != "succeeded" {
		t.Fatalf("clean exit not carried through: %+v", in)
	}
}

func TestEveryExitARunnerCanReadIsAccepted(t *testing.T) {
	_, cat := offlineTestEntry(t)
	for _, code := range []int{0, 1, 2, 127, 128, 255, 0xC0000005, 0xC000013A} {
		entry := exited(t, code, code)
		in, err := offlineInput(entry, cat)
		if err != nil {
			t.Fatalf("exit %d was refused: %v", code, err)
		}
		if in.ChildExitCode != code {
			t.Fatalf("exit %d stored as %d", code, in.ChildExitCode)
		}
	}
}

// TestTheWidestDWORDIsAccepted pins the ceiling itself rather than a number
// near it: Windows states the whole DWORD, so the largest one is a code a
// runner can genuinely read out of a child.
func TestTheWidestDWORDIsAccepted(t *testing.T) {
	if int64(int(widestLocallyReportedExit)) != widestLocallyReportedExit {
		t.Skip("int is narrower than the widest DWORD on this build")
	}
	_, cat := offlineTestEntry(t)
	widest := int(widestLocallyReportedExit)
	if _, err := offlineInput(exited(t, widest, widest), cat); err != nil {
		t.Fatalf("the widest reportable exit was refused: %v", err)
	}
}

// TestTheSentinelStaysAcceptable pins the case an "exit codes are not
// negative" rule would get wrong. A spooled record has no optional field to
// leave out, and the executor writes -1 for work no child decided, so the
// sentinel is the honest report rather than a contradiction.
func TestTheSentinelStaysAcceptable(t *testing.T) {
	_, cat := offlineTestEntry(t)
	in, err := offlineInput(exited(t, noLocalChildExit, noLocalChildExit), cat)
	if err != nil {
		t.Fatalf("the no-child sentinel was refused: %v", err)
	}
	if in.ChildExitCode != noLocalChildExit {
		t.Fatalf("sentinel stored as %d", in.ChildExitCode)
	}
	if in.Result != "failed" {
		t.Fatalf("a record no child decided was read as %q", in.Result)
	}
}

func TestAnAttemptExitBelowTheSentinelIsRefused(t *testing.T) {
	for _, code := range []int{-2, -255, math.MinInt32} {
		refused(t, exited(t, code, 0), "an attempt exit below the sentinel")
	}
}

func TestAStepExitBelowTheSentinelIsRefused(t *testing.T) {
	for _, code := range []int{-2, -255, math.MinInt32} {
		refused(t, exited(t, 0, code), "a step exit below the sentinel")
	}
}

func TestAnExitWiderThanARunnerCanReadIsRefused(t *testing.T) {
	if int64(int(widestLocallyReportedExit)) != widestLocallyReportedExit {
		t.Skip("int is narrower than the widest DWORD on this build")
	}
	tooWide := int(widestLocallyReportedExit) + 1
	refused(t, exited(t, tooWide, 0), "an attempt exit wider than a DWORD")
	refused(t, exited(t, 0, tooWide), "a step exit wider than a DWORD")
}

// TestAnUnreadableExitIsRefusedRatherThanReadAsAFailure states what the
// refusal buys. The switch below reads any non-zero code as an ordinary
// failure, so without this rule a number no process ever returned lands in
// durable history looking exactly like an exit a child really gave.
func TestAnUnreadableExitIsRefusedRatherThanReadAsAFailure(t *testing.T) {
	if int64(int(widestLocallyReportedExit)) != widestLocallyReportedExit {
		t.Skip("int is narrower than the widest DWORD on this build")
	}
	refused(t, exited(t, int(widestLocallyReportedExit)+1, 0), "an exit no runner could read")
	refused(t, exited(t, math.MinInt32, 0), "an exit far below the sentinel")
}

// TestTheRefusalNamesTheStepItJudged keeps the message useful: a record states
// one code per step and an operator has to know which one was refused.
func TestTheRefusalNamesTheStepItJudged(t *testing.T) {
	entry := exited(t, 0, math.MinInt32)
	err := checkLocalExitCodesNameAChildThatRan(entry)
	if err == nil {
		t.Fatal("a step exit below the sentinel was accepted")
	}
	name := entry.Result.Steps[0].Name
	if !strings.Contains(err.Error(), name) {
		t.Fatalf("refusal %q does not name step %q", err, name)
	}
}

func TestARecordWithNoResultIsRefusedHere(t *testing.T) {
	entry, _ := offlineTestEntry(t)
	entry.Result = nil
	if err := checkLocalExitCodesNameAChildThatRan(entry); err == nil {
		t.Fatal("a record stating no result was accepted")
	} else if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("refusal does not travel as invalid: %v", err)
	}
}
