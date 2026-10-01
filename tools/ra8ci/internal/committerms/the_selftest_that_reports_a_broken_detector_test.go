// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package committerms

import (
	"bytes"
	"context"
	"regexp"
	"strings"
	"testing"
)

// The self-test is the only thing standing between a detector that quietly
// stopped matching and a gate that reports every commit clean. Its three
// assertions are worth nothing unless each one actually fails loudly, so
// each is driven here against a detector deliberately broken in exactly the
// way that assertion exists to catch.
//
// The table is swapped in-package and restored, which is the only seam:
// the self-test takes no input but its writers, and its fixture messages
// are compiled in.
func withDetector(t *testing.T, patterns []termPattern) {
	t.Helper()
	original := banned
	banned = patterns
	t.Cleanup(func() { banned = original })
}

func runSelfTest(t *testing.T) (bool, string, string) {
	t.Helper()
	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	held := selfTest(stdout, stderr)
	return held, stdout.String(), stderr.String()
}

// A detector that matches nothing reports every commit clean. That is the
// failure the first assertion exists for, and it must be named rather than
// passed over.
func TestADetectorThatFiresOnNothingFailsTheSelfTest(t *testing.T) {
	withDetector(t, nil)

	held, stdout, stderr := runSelfTest(t)
	if held {
		t.Fatal("a detector matching nothing passed its own self-test")
	}
	if !strings.Contains(stderr, "un-annotated MOSI") {
		t.Fatalf("the failure did not say what stopped firing: %q", stderr)
	}
	if stdout != "" {
		t.Fatalf("a failed self-test still announced itself: %q", stdout)
	}
}

// The opt-out is the widest escape hatch in the gate, and the second
// assertion guards the other direction: a detector so eager that an excused
// paragraph still reports must fail, and must print the reports it should
// not have made.
func TestADetectorThatReportsAnExcusedParagraphFailsTheSelfTest(t *testing.T) {
	withDetector(t, append(append([]termPattern{}, banned...),
		termPattern{regexp.MustCompile(`widen`), "widen -- fixture term"}))

	held, stdout, stderr := runSelfTest(t)
	if held {
		t.Fatal("a detector reporting through an opt-out passed its own self-test")
	}
	if !strings.Contains(stderr, "paragraph-scoped LEGACY-OK") {
		t.Fatalf("the failure did not name the opt-out: %q", stderr)
	}
	if !strings.Contains(stderr, "widen -- fixture term") {
		t.Fatalf("the failure did not print what was wrongly reported: %q", stderr)
	}
	if stdout != "" {
		t.Fatalf("a failed self-test still announced itself: %q", stdout)
	}
}

// The third assertion is the leak check: one paragraph's LEGACY-OK must not
// excuse another's. A detector that matches only in the first fixture lets
// the cross-paragraph case come back empty, which is indistinguishable from
// the leak, and the self-test has to refuse it.
func TestADetectorThatMissesTheCrossParagraphCaseFailsTheSelfTest(t *testing.T) {
	withDetector(t, []termPattern{{regexp.MustCompile(`MOSI/MISO`), "MOSI/MISO -- fixture term"}})

	held, stdout, stderr := runSelfTest(t)
	if held {
		t.Fatal("a detector blind to the cross-paragraph case passed its own self-test")
	}
	if !strings.Contains(stderr, "different paragraph") {
		t.Fatalf("the failure did not name the leak it checks for: %q", stderr)
	}
	if stdout != "" {
		t.Fatalf("a failed self-test still announced itself: %q", stdout)
	}
}

// The intact detector still holds, so the three cases above are the
// detector being broken and not the fixtures having rotted.
func TestTheDetectorAsShippedPassesItsOwnSelfTest(t *testing.T) {
	held, stdout, stderr := runSelfTest(t)
	if !held {
		t.Fatalf("the shipped detector failed its own self-test: %q", stderr)
	}
	if !strings.Contains(stdout, "[SELFTEST OK]") || stderr != "" {
		t.Fatalf("a passing self-test did not announce itself cleanly: stdout=%q stderr=%q", stdout, stderr)
	}
}

// End to end: --selftest carries the verdict out as the exit code, so a
// broken detector stops CI at the gate rather than at a commit.
func TestSelfTestAnswersOneWhenTheDetectorIsBroken(t *testing.T) {
	withDetector(t, nil)

	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	code := Run(context.Background(), []string{"--selftest"}, strings.NewReader(""), stdout, stderr)
	if code != 1 {
		t.Fatalf("a broken detector answered %d, not 1", code)
	}
	if !strings.Contains(stderr.String(), "[SELFTEST FAIL]") {
		t.Fatalf("the exit code was not explained: %q", stderr.String())
	}
	if stdout.String() != "" {
		t.Fatalf("a broken detector still wrote to stdout: %q", stdout.String())
	}
}
