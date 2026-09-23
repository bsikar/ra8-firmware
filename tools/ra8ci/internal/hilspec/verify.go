// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"bytes"
	"errors"
	"fmt"
	"regexp"
)

var (
	ErrUnsupportedCaptureMode = errors.New("HIL mode does not use text capture assertions")
	ErrMissingExpectation     = errors.New("HIL text capture has no positive expectation")
	ErrWeakExpectation        = errors.New("HIL positive expectation is shorter than 12 bytes")
	ErrExpectationNotFound    = errors.New("HIL positive expectation was not found")
	ErrNegativeExpectation    = errors.New("HIL negative expectation matched")
)

// VerifyTextCapture applies the repository's UART/RTT assertion contract to
// captured bytes. The positive expectation is a literal byte substring;
// HIL_EXPECT_NEGATIVE is a Go/RE2 regular expression and must not match.
// Hardware capture and firmware failure-banner analysis remain separate.
func VerifyTextCapture(spec Spec, captured []byte) error {
	if spec.Mode != ModeUARTScrape && spec.Mode != ModeRTTScrape {
		return ErrUnsupportedCaptureMode
	}
	if spec.Expect == "" {
		return ErrMissingExpectation
	}
	if len(spec.Expect) < 12 && !spec.Values["HIL_EXPECT_SHORT_OK"].Flag {
		return fmt.Errorf("%w: %d bytes", ErrWeakExpectation, len(spec.Expect))
	}
	if !bytes.Contains(captured, []byte(spec.Expect)) {
		return ErrExpectationNotFound
	}
	if spec.ExpectNegative != "" {
		// UART uses grep -iE; RTT uses grep -qE. Preserve that distinction.
		negative := spec.ExpectNegative
		if spec.Mode == ModeUARTScrape {
			negative = "(?i:(?:" + negative + "))"
		}
		pattern, err := regexp.Compile(negative)
		if err != nil {
			return fmt.Errorf("invalid HIL negative expectation: %w", err)
		}
		if pattern.Match(captured) {
			return ErrNegativeExpectation
		}
	}
	return nil
}
