// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package stubcryptoguard

import (
	"bytes"
	"regexp"
	"strings"
	"testing"
)

// The self-test runs both directions over a fixture it writes itself: a
// guarded stub with a fail-closed else must stay quiet, and one whose else
// returns success with the insecure token escaped outside the guard must
// fire twice. Both verdicts are read through the directive patterns, so a
// pattern that stops recognizing its preprocessor form takes the detector
// blind and the self-test's answers stop meaning anything.
//
// Until now neither failure branch had ever run, which left the guard's
// own alarm untested. Each is driven here by blinding one pattern, the
// patterns being package-level vars and the only seam: the fixtures and
// their expectations are compiled in.
var neverMatches = regexp.MustCompile(`$^`)

func withDirective(t *testing.T, target **regexp.Regexp, replacement *regexp.Regexp) {
	t.Helper()
	original := *target
	*target = replacement
	t.Cleanup(func() { *target = original })
}

func selfTestSays(t *testing.T) (bool, string, string) {
	t.Helper()
	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	held := selfTest(stdout, stderr)
	return held, stdout.String(), stderr.String()
}

// A guard opener nobody recognizes leaves the whole fixture looking
// unguarded, so both directions stop describing the file in front of them.
func TestASelfTestWithAnUnrecognizedGuardFailsLoudly(t *testing.T) {
	withDirective(t, &guardDirective, neverMatches)

	held, stdout, stderr := selfTestSays(t)
	if held {
		t.Fatal("a detector blind to its own guard opener passed its self-test")
	}
	if !strings.Contains(stderr, "[FAIL]") {
		t.Fatalf("the failing direction was not named: %q", stderr)
	}
	if !strings.Contains(stderr, "failure(s)") {
		t.Fatalf("the failures were not counted: %q", stderr)
	}
	if strings.Contains(stdout, "all cases pass") {
		t.Fatalf("a failed self-test still reported both directions passing: %q", stdout)
	}
}

// The else branch is where fail-closed behaviour is read, so a pattern
// blind to it is the difference between a stub that refuses on target and
// one that returns success. The self-test has to refuse that too.
func TestASelfTestWithAnUnrecognizedElseFailsLoudly(t *testing.T) {
	withDirective(t, &elseDirective, neverMatches)

	held, stdout, stderr := selfTestSays(t)
	if held {
		t.Fatal("a detector blind to the else branch passed its self-test")
	}
	if !strings.Contains(stderr, "[FAIL]") {
		t.Fatalf("the failing direction was not named: %q", stderr)
	}
	if strings.Contains(stdout, "all cases pass") {
		t.Fatalf("a failed self-test still reported both directions passing: %q", stdout)
	}
}

// The control: with every pattern as shipped, both directions pass and say
// so. That is what makes the two failures above the detector being blinded
// rather than the fixtures having rotted.
func TestTheDetectorAsShippedPassesBothDirections(t *testing.T) {
	held, stdout, stderr := selfTestSays(t)
	if !held {
		t.Fatalf("the shipped detector failed its own self-test: %q", stderr)
	}
	if !strings.Contains(stdout, "all cases pass") {
		t.Fatalf("a passing self-test did not say so: %q", stdout)
	}
	if strings.Contains(stderr, "[FAIL]") {
		t.Fatalf("a passing self-test still reported a failure: %q", stderr)
	}
}
