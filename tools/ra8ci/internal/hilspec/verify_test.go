// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"errors"
	"testing"
)

func TestVerifyTextCaptureAppliesPositiveAndNegativeAssertions(t *testing.T) {
	spec := Spec{Mode: ModeUARTScrape, Expect: "verdict=PASS", ExpectNegative: `HardFault|verdict=FAIL`}
	if err := VerifyTextCapture(spec, []byte("boot\nverdict=PASS\nready\n")); err != nil {
		t.Fatalf("valid output rejected: %v", err)
	}
	if err := VerifyTextCapture(spec, []byte("boot\nverdict=FAIL\n")); !errors.Is(err, ErrExpectationNotFound) {
		t.Fatalf("missing positive assertion was not rejected first: %v", err)
	}
	if err := VerifyTextCapture(spec, []byte("verdict=PASS\nHardFault\n")); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("negative assertion match was accepted: %v", err)
	}
}

func TestVerifyTextCapturePreservesModeSpecificNegativeCaseBehavior(t *testing.T) {
	uart := Spec{Mode: ModeUARTScrape, Expect: "verdict=PASS", ExpectNegative: "HardFault"}
	if err := VerifyTextCapture(uart, []byte("verdict=PASS\nhardfault\n")); !errors.Is(err, ErrNegativeExpectation) {
		t.Fatalf("UART negative check did not match case-insensitively: %v", err)
	}
	rtt := Spec{Mode: ModeRTTScrape, Expect: "verdict=PASS", ExpectNegative: "HardFault"}
	if err := VerifyTextCapture(rtt, []byte("verdict=PASS\nhardfault\n")); err != nil {
		t.Fatalf("RTT negative check unexpectedly ignored case: %v", err)
	}
}

func TestVerifyTextCaptureRequiresStrongPositiveAndValidRegex(t *testing.T) {
	spec := Spec{Mode: ModeRTTScrape, Expect: "PASS"}
	if err := VerifyTextCapture(spec, []byte("PASS")); !errors.Is(err, ErrWeakExpectation) {
		t.Fatalf("weak expectation was accepted: %v", err)
	}
	spec.Values = map[string]Value{"HIL_EXPECT_SHORT_OK": {Kind: FlagValue, Flag: true}}
	if err := VerifyTextCapture(spec, []byte("PASS")); err != nil {
		t.Fatalf("explicit short expectation was rejected: %v", err)
	}
	spec.ExpectNegative = "["
	if err := VerifyTextCapture(spec, []byte("PASS")); err == nil {
		t.Fatal("invalid negative regular expression was accepted")
	}
}

func TestVerifyTextCaptureRejectsWrongModesAndMissingExpectation(t *testing.T) {
	if err := VerifyTextCapture(Spec{Mode: ModeAlive, Expect: "long-enough-pass"}, []byte("long-enough-pass")); !errors.Is(err, ErrUnsupportedCaptureMode) {
		t.Fatalf("non-text mode accepted text capture assertions: %v", err)
	}
	if err := VerifyTextCapture(Spec{Mode: ModeUARTScrape}, nil); !errors.Is(err, ErrMissingExpectation) {
		t.Fatalf("missing expectation accepted: %v", err)
	}
}
