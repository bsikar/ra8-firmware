// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"errors"
	"regexp"
	"strings"
	"testing"
)

const manifestUnderTest = "examples/ek_ra8d2/hw_validated/hil/uart_hello/hil.conf"

// brokenPatterns are strings a manifest can carry that are not regular
// expressions. Every one of them is writable through the assignment grammar,
// so each is a manifest somebody can actually commit.
var brokenPatterns = []string{
	"[HardFault",
	"(HardFault",
	"a{2,1}",
	"*HardFault",
	"(?P<",
	"HardFault\\",
}

// contractPatterns are the shapes the repository's manifests use today.
var contractPatterns = []string{
	"HardFault",
	"^HardFault",
	"(HardFault|BusFault)",
	"[Ee]rror",
	"verdict=FAIL",
	"0x[0-9a-f]{8}",
	"^(?:panic|abort)",
}

// manifestWith builds the smallest manifest that carries a negative
// expectation, so a test varies one line and nothing else.
func manifestCarrying(mode Mode, negative string) string {
	body := "HIL_MODE=" + string(mode) + "\n" + "HIL_EXPECT=\"verdict=PASS ready\"\n"
	if negative != "" {
		body += "HIL_EXPECT_NEGATIVE=\"" + negative + "\"\n"
	}
	return body
}

func parseNegativeManifest(t *testing.T, mode Mode, negative string) (Spec, error) {
	t.Helper()
	return Parse(strings.NewReader(manifestCarrying(mode, negative)), manifestUnderTest)
}

func TestAManifestWhoseNegativeExpectationIsNotAPatternIsRefused(t *testing.T) {
	for _, pattern := range brokenPatterns {
		spec, err := parseNegativeManifest(t, ModeUARTScrape, pattern)
		if !errors.Is(err, ErrInvalidManifest) {
			t.Fatalf("%q was accepted as a negative expectation: spec=%+v err=%v", pattern, spec, err)
		}
		if !strings.Contains(err.Error(), manifestUnderTest) {
			t.Fatalf("%q was refused without naming the manifest to fix: %v", pattern, err)
		}
	}
}

func TestTheNegativeExpectationsTheContractUsesAreStillAccepted(t *testing.T) {
	for _, pattern := range contractPatterns {
		spec, err := parseNegativeManifest(t, ModeUARTScrape, pattern)
		if err != nil {
			t.Fatalf("%q is a pattern the verdict can apply and was refused: %v", pattern, err)
		}
		if spec.ExpectNegative != pattern {
			t.Fatalf("%q reached the spec as %q", pattern, spec.ExpectNegative)
		}
	}
}

func TestABrokenPatternIsRefusedUnderEveryMode(t *testing.T) {
	modes := []Mode{ModeAlive, ModeUARTScrape, ModeRTTScrape, ModeJLinkMemprobe, ModeEthernetTCP, ModeC6CameraLivestream}
	for _, mode := range modes {
		if _, err := parseNegativeManifest(t, mode, "HardFault"); err != nil {
			t.Fatalf("%s refused a pattern the verdict can apply: %v", mode, err)
		}
		if _, err := parseNegativeManifest(t, mode, "[HardFault"); !errors.Is(err, ErrInvalidManifest) {
			t.Fatalf("%s accepted a value that is not a pattern: %v", mode, err)
		}
	}
}

func TestChangingOnlyTheNegativeExpectationFlipsTheDoor(t *testing.T) {
	accepted, err := parseNegativeManifest(t, ModeRTTScrape, "HardFault")
	if err != nil {
		t.Fatalf("the baseline manifest was refused: %v", err)
	}
	if _, err := parseNegativeManifest(t, ModeRTTScrape, "HardFault["); !errors.Is(err, ErrInvalidManifest) {
		t.Fatalf("one character of difference did not reach the door: %v", err)
	}
	if accepted.Mode != ModeRTTScrape || accepted.Expect != "verdict=PASS ready" || len(accepted.Values) != 3 {
		t.Fatalf("the rule changed a manifest it accepted: %+v", accepted)
	}
}

func TestAManifestWithNoNegativeExpectationIsUntouched(t *testing.T) {
	spec, err := parseNegativeManifest(t, ModeUARTScrape, "")
	if err != nil {
		t.Fatalf("a manifest declaring no negative expectation was refused: %v", err)
	}
	if spec.ExpectNegative != "" || len(spec.Values) != 2 {
		t.Fatalf("a manifest declaring no negative expectation gained one: %+v", spec)
	}
}

// TestTheParseDoorAgreesWithTheVerdictDoor is the point of the rule: the
// verdict may refuse a capture, but it may never refuse the manifest.
func TestTheParseDoorAgreesWithTheVerdictDoor(t *testing.T) {
	capture := []byte("boot\nverdict=PASS ready\nHardFault at 0x20001000\n")
	for _, mode := range []Mode{ModeUARTScrape, ModeRTTScrape} {
		for _, pattern := range append(append([]string{}, contractPatterns...), brokenPatterns...) {
			_, parseErr := parseNegativeManifest(t, mode, pattern)
			spec := Spec{Path: manifestUnderTest, Mode: mode, Expect: "verdict=PASS ready", ExpectNegative: pattern}
			verifyErr := VerifyTextCapture(spec, capture)
			unapplicable := verifyErr != nil && strings.Contains(verifyErr.Error(), "invalid HIL negative expectation")
			if (parseErr != nil) != unapplicable {
				t.Fatalf("%s %q: parse err=%v but the verdict %v", mode, pattern, parseErr, verifyErr)
			}
			if parseErr == nil && unapplicable {
				t.Fatalf("%s %q reached the board and then failed on the manifest: %v", mode, pattern, verifyErr)
			}
		}
	}
}

// TestAnAcceptedManifestOnlyEverFailsOnItsCapture pins the other half: an
// accepted pattern ends in a verdict, clean or not, never in a parse error.
func TestAnAcceptedManifestOnlyEverFailsOnItsCapture(t *testing.T) {
	capture := []byte("boot\nverdict=PASS ready\nHardFault at 0x20001000\n")
	for _, pattern := range contractPatterns {
		spec, err := parseNegativeManifest(t, ModeUARTScrape, pattern)
		if err != nil {
			t.Fatalf("%q was refused: %v", pattern, err)
		}
		verifyErr := VerifyTextCapture(spec, capture)
		if verifyErr != nil && !errors.Is(verifyErr, ErrNegativeExpectation) {
			t.Fatalf("%q ended in something other than a verdict: %v", pattern, verifyErr)
		}
	}
}

// TestATranscribedWrapperDecidesTheSameCases judges the door against a
// transcription of the wrapper the verdict compiles with, rather than against
// the helper under test.
func TestATranscribedWrapperDecidesTheSameCases(t *testing.T) {
	transcribed := func(mode Mode, pattern string) error {
		flags := "m"
		if mode == ModeUARTScrape {
			flags = "im"
		}
		_, err := regexp.Compile("(?" + flags + ":(?:" + pattern + "))")
		return err
	}
	patterns := append(append([]string{}, contractPatterns...), brokenPatterns...)
	patterns = append(patterns, "a|b", "^(a|b)", "[", "]", "a**", "(?i)HardFault")
	for _, mode := range []Mode{ModeAlive, ModeUARTScrape, ModeRTTScrape, ModeEthernetTCP} {
		for _, pattern := range patterns {
			_, parseErr := parseNegativeManifest(t, mode, pattern)
			if (parseErr != nil) != (transcribed(mode, pattern) != nil) {
				t.Fatalf("%s %q: door says %v, the wrapper the verdict uses says %v",
					mode, pattern, parseErr, transcribed(mode, pattern))
			}
		}
	}
}
