// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"errors"
	"testing"
)

func capturing(mode Mode, negative string) Spec {
	return Spec{Mode: mode, Expect: "verdict=PASS", ExpectNegative: negative}
}

func TestALineAnchoredNegativeExpectationMatchesTheLineGrepWouldMatch(t *testing.T) {
	capture := []byte("boot\nverdict=PASS\nHardFault at 0x2000\n")
	for _, pattern := range []string{"^HardFault", "HardFault at 0x2000$", "^HardFault at 0x2000$"} {
		if err := VerifyTextCapture(capturing(ModeUARTScrape, pattern), capture); !errors.Is(err, ErrNegativeExpectation) {
			t.Fatalf("UART %q did not match the line that carries it: %v", pattern, err)
		}
		if err := VerifyTextCapture(capturing(ModeRTTScrape, pattern), capture); !errors.Is(err, ErrNegativeExpectation) {
			t.Fatalf("RTT %q did not match the line that carries it: %v", pattern, err)
		}
	}
}

func TestAnAnchoredNegativeExpectationStillMissesTheLinesThatDoNotCarryIt(t *testing.T) {
	capture := []byte("boot\nverdict=PASS\nno fault here\n")
	for _, pattern := range []string{"^HardFault", "HardFault$", "^$"} {
		if err := VerifyTextCapture(capturing(ModeUARTScrape, pattern), capture); err != nil {
			t.Fatalf("UART %q matched a capture that never carries it: %v", pattern, err)
		}
	}
}

func TestTheTerminatorOfTheLastLineIsNotAnEmptyLine(t *testing.T) {
	if err := VerifyTextCapture(capturing(ModeRTTScrape, "^$"), []byte("boot\nverdict=PASS\n")); err != nil {
		t.Fatalf("the newline ending the last line was read as an empty line: %v", err)
	}
	if err := VerifyTextCapture(capturing(ModeRTTScrape, "^$"), []byte("verdict=PASS\n\nboot\n")); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("a capture that really does carry an empty line was cleared: %v", err)
	}
}

func TestTheFinalLineIsJudgedWithoutATrailingNewline(t *testing.T) {
	if err := VerifyTextCapture(capturing(ModeRTTScrape, "HardFault$"), []byte("verdict=PASS\nHardFault")); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("an unterminated last line escaped the negative expectation: %v", err)
	}
}

func TestLineAnchoringKeepsEachModeCaseBehaviour(t *testing.T) {
	capture := []byte("verdict=PASS\nHardFault\n")
	if err := VerifyTextCapture(capturing(ModeUARTScrape, "^hardfault$"), capture); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("UART anchored negative check lost its case insensitivity: %v", err)
	}
	if err := VerifyTextCapture(capturing(ModeRTTScrape, "^hardfault$"), capture); err != nil {
		t.Fatalf("RTT anchored negative check picked up case insensitivity: %v", err)
	}
	if err := VerifyTextCapture(capturing(ModeRTTScrape, "^HardFault$"), capture); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("RTT anchored negative check did not match its own case: %v", err)
	}
}

func TestAlternationStaysWholePatternAlternation(t *testing.T) {
	capture := []byte("verdict=PASS\nHardFault\n")
	if err := VerifyTextCapture(capturing(ModeUARTScrape, "^verdict=FAIL$|^HardFault$"), capture); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("the second alternative was not tried: %v", err)
	}
	if err := VerifyTextCapture(capturing(ModeRTTScrape, "HardFault|verdict=FAIL"), capture); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("an unanchored alternation stopped matching: %v", err)
	}
}

func TestADotStillDoesNotCrossALine(t *testing.T) {
	if err := VerifyTextCapture(capturing(ModeRTTScrape, "verdict=PASS.HardFault"), []byte("verdict=PASS\nHardFault\n")); err != nil {
		t.Fatalf("a dot matched a newline, so the negative expectation spans lines no grep would: %v", err)
	}
}

func TestAnUncompilableNegativeExpectationIsStillRefused(t *testing.T) {
	for _, pattern := range []string{"[", ")", "*", "(?P<x"} {
		err := VerifyTextCapture(capturing(ModeRTTScrape, pattern), []byte("verdict=PASS"))
		if err == nil || errors.Is(err, ErrNegativeExpectation) {
			t.Fatalf("%q was not refused as an invalid expression: %v", pattern, err)
		}
	}
}

func TestTheWrapperIsTheOnlyDifferenceBetweenTheModes(t *testing.T) {
	uart, err := compileNegativeExpectation(ModeUARTScrape, "HardFault")
	if err != nil {
		t.Fatalf("UART pattern did not compile: %v", err)
	}
	rtt, err := compileNegativeExpectation(ModeRTTScrape, "HardFault")
	if err != nil {
		t.Fatalf("RTT pattern did not compile: %v", err)
	}
	if uart.String() != "(?im:(?:HardFault))" {
		t.Fatalf("UART wrapper changed: %s", uart.String())
	}
	if rtt.String() != "(?m:(?:HardFault))" {
		t.Fatalf("RTT wrapper changed: %s", rtt.String())
	}
}
