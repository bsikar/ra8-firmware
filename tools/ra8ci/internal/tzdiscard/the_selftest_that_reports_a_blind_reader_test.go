// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	"bytes"
	"regexp"
	"strings"
	"testing"
)

// Both rules this gate enforces are read through package-level patterns, so
// a pattern that stops recognizing a discarded call reads a boot
// translation unit full of ignored error codes as clean. The self-test runs
// both directions over text it holds itself and is what stands between
// that and a green run, yet neither of its failure branches had ever run:
// the [FAIL] line and the failure summary were untested, which left the
// alarm unproven.

func withFamilyPattern(t *testing.T, replacement *regexp.Regexp) {
	t.Helper()
	original := familyPattern
	familyPattern = replacement
	t.Cleanup(func() { familyPattern = original })
}

func withWaiverPattern(t *testing.T, replacement *regexp.Regexp) {
	t.Helper()
	original := waiverPattern
	waiverPattern = replacement
	t.Cleanup(func() { waiverPattern = original })
}

func selfTested(t *testing.T) (bool, string, string) {
	t.Helper()
	stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
	held := selfTest(stdout, stderr)
	return held, stdout.String(), stderr.String()
}

// Rule A is the world-switch family. Blind its pattern and the discarded
// ra8_tz_secure_boot_verify() call in the fixture stops being a finding,
// so only one of the two rules fires and the self-test has to name the
// direction that went quiet.
func TestASelfTestWithABlindFamilyReaderNamesTheRuleThatWentQuiet(t *testing.T) {
	withFamilyPattern(t, regexp.MustCompile(`ra8_tz_never_written_this_way\s*\(`))

	held, stdout, stderr := selfTested(t)
	if held {
		t.Fatal("a reader blind to the world-switch family passed its self-test")
	}
	if !strings.Contains(stderr, "[FAIL] world-switch and boot-translation-unit discards both fire") {
		t.Fatalf("the quiet rule was not named: %q", stderr)
	}
	if !strings.Contains(stderr, "failure(s)") {
		t.Fatalf("the failure was not counted: %q", stderr)
	}
	if strings.Contains(stdout, "all cases pass") {
		t.Fatalf("a failed self-test still reported both directions passing: %q", stdout)
	}
}

// The other direction. A waiver pattern that matches nothing makes the
// documented fallback in the clean fixture read as a violation, so a gate
// carrying this change would refuse code that followed the rule. The
// self-test has to catch that too.
func TestASelfTestThatNoLongerHonoursAWaiverFails(t *testing.T) {
	withWaiverPattern(t, regexp.MustCompile(`TZ-DISCARD-NEVER-OK:\s*\S`))

	held, _, stderr := selfTested(t)
	if held {
		t.Fatal("a gate that ignores its own waiver marker passed its self-test")
	}
	if !strings.Contains(stderr, "[FAIL] handled results and exact reasoned waiver stay quiet") {
		t.Fatalf("the quiet direction was not named: %q", stderr)
	}
}

// The control: with both patterns as shipped the self-test passes and says
// so, which is what makes the two failures above the readers being swapped
// rather than the compiled-in fixtures having rotted.
func TestTheReadersAsShippedPassBothDirections(t *testing.T) {
	held, stdout, stderr := selfTested(t)
	if !held {
		t.Fatalf("the shipped readers failed their own self-test: %q", stderr)
	}
	if !strings.Contains(stdout, "all cases pass") {
		t.Fatalf("a passing self-test did not say so: %q", stdout)
	}
	if strings.Contains(stderr, "[FAIL]") {
		t.Fatalf("a passing self-test still reported a failure: %q", stderr)
	}
}

// A mention welded to a longer identifier is not the entry point, and the
// scan has to read past it to the next mention rather than stopping there.
// A file whose only other mention is a declaration never boots anything,
// so the wide rule stays off.
func TestAWeldedMentionIsPassedOverAndTheWideRuleStaysOff(t *testing.T) {
	if definesBootEntry("void my_SystemInit(void) { halt(); }\nvoid SystemInit(void);\n") {
		t.Fatal("a welded name plus a declaration switched the wide rule on")
	}
}

// The same file with a real definition after the welded mention does boot,
// so the pass-over above is the weld being recognized rather than the scan
// giving up at the first mention it cannot use.
func TestTheScanReadsPastAWeldedMentionToARealDefinition(t *testing.T) {
	if !definesBootEntry("void my_SystemInit(void) { halt(); }\nvoid SystemInit(void) { boot(); }\n") {
		t.Fatal("the scan stopped at the welded mention instead of reading on")
	}
}
