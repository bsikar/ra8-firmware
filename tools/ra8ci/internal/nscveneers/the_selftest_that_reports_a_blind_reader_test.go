// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	"bytes"
	"context"
	"regexp"
	"strings"
	"testing"
)

// The self-test runs both directions over text it holds itself: a veneer
// declared and defined must stay quiet, and one declared but only called
// must fire. Both verdicts are read through the declaration pattern, so a
// pattern that stops recognizing the annotation reads the header as
// declaring nothing, and a gate that finds no veneers reports no missing
// definitions. That is the quietest way this gate can fail.
//
// Neither failure branch had ever run, which left the alarm untested. The
// pattern is a package-level var and the only seam: the header and source
// text and their expectations are compiled in.
func withDeclarationPattern(t *testing.T, replacement *regexp.Regexp) {
	t.Helper()
	original := declaration
	declaration = replacement
	t.Cleanup(func() { declaration = original })
}

func selfTestSays(t *testing.T) (bool, string, string) {
	t.Helper()
	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	held := selfTest(stdout, stderr)
	return held, stdout.String(), stderr.String()
}

// A pattern that matches nothing reads the boundary as publishing no
// veneers at all, which would let a phantom through silently. Both
// directions must be named as failing and counted.
func TestASelfTestWithABlindDeclarationReaderFailsLoudly(t *testing.T) {
	withDeclarationPattern(t, regexp.MustCompile(`$^`))

	held, stdout, stderr := selfTestSays(t)
	if held {
		t.Fatal("a reader blind to the veneer annotation passed its self-test")
	}
	if !strings.Contains(stderr, "[FAIL] matching veneer definition stays quiet") ||
		!strings.Contains(stderr, "[FAIL] call-only phantom veneer fires") {
		t.Fatalf("both directions should have been named as failing: %q", stderr)
	}
	if !strings.Contains(stderr, "failure(s)") {
		t.Fatalf("the failures were not counted: %q", stderr)
	}
	if strings.Contains(stdout, "all cases pass") {
		t.Fatalf("a failed self-test still reported both directions passing: %q", stdout)
	}
}

// A pattern that captures the wrong text is worse than one that matches
// nothing, because the gate then looks busy while checking names no header
// ever published. The self-test has to refuse that too, and --selftest has
// to carry the refusal out as exit 1.
func TestSelfTestAnswersOneWhenTheReaderCapturesTheWrongName(t *testing.T) {
	withDeclarationPattern(t, regexp.MustCompile(`RA8_NSC_VENEER\s+(\w+)`))

	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	if code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, stdout, stderr); code != 1 {
		t.Fatalf("a misreading gate answered %d, not 1", code)
	}
	if !strings.Contains(stderr.String(), "[FAIL]") {
		t.Fatalf("the exit code was not explained: %q", stderr.String())
	}
	if strings.Contains(stdout.String(), "all cases pass") {
		t.Fatalf("a failed self-test still reported passing: %q", stdout.String())
	}
}

// The control: with the shipped pattern both directions pass and say so,
// which is what makes the two failures above the reader being blinded
// rather than the fixture text having rotted.
func TestTheReaderAsShippedPassesBothDirections(t *testing.T) {
	held, stdout, stderr := selfTestSays(t)
	if !held {
		t.Fatalf("the shipped reader failed its own self-test: %q", stderr)
	}
	if !strings.Contains(stdout, "all cases pass") {
		t.Fatalf("a passing self-test did not say so: %q", stdout)
	}
	if strings.Contains(stderr, "[FAIL]") {
		t.Fatalf("a passing self-test still reported a failure: %q", stderr)
	}
}
