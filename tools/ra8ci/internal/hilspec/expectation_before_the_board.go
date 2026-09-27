// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import "fmt"

// minimumExpectationBytes is the floor a positive expectation has to clear
// before HIL_EXPECT_SHORT_OK is needed. VerifyTextCapture reads the same
// constant, so the parse door and the verdict door cannot drift apart about
// what counts as too short to be evidence.
const minimumExpectationBytes = 12

// checkPositiveExpectationIsUsable refuses a text-capture manifest whose
// HIL_EXPECT cannot produce a verdict, before a board is reserved for it.
//
// This is checkNegativeExpectationCompiles applied to the other half of the
// assertion. VerifyTextCapture holds the positive expectation to two things:
// it has to exist (ErrMissingExpectation) and it has to be long enough to be
// evidence rather than a coincidence, unless the manifest says the short form
// is deliberate (ErrWeakExpectation, HIL_EXPECT_SHORT_OK). Both are
// properties of the manifest alone. Neither needs a capture, a board, or a
// run to decide. Both were nonetheless discovered at the verdict.
//
// By then the run has cost what a HIL run costs: the board leased, the
// fixture neutralized, the image flashed, the observation window spent whole,
// the capture taken. And the attempt ends carrying a complaint about its
// manifest where its verdict should be, charged to firmware that may have
// been perfect. A missing expectation is the worse of the two: under
// uart_scrape and rtt_scrape the expectation IS the verdict, so the manifest
// asks for a board in order to decide nothing.
//
// Unlike the negative expectation, which is a malformed value and refused
// under every mode, this one is scoped to the two modes that read it. An
// absent HIL_EXPECT means nothing under jlink_memprobe or hil_eth_tcp:
// VerifyTextCapture turns those away at the door with
// ErrUnsupportedCaptureMode, they reach their verdict by other means, and
// examples/ra8p1_foundation/blink_ra8p1 is a jlink_memprobe manifest with no
// expectation today. Absence is only a defect where something reads it,
// which is the same line phase_fits_cap.go draws.
//
// VerifyTextCapture keeps both of its own checks. It takes a Spec, not a
// path, and a Spec can be built by a caller that never went through Parse, so
// the verdict door stays closed on its own terms.
func checkPositiveExpectationIsUsable(spec Spec) error {
	if spec.Mode != ModeUARTScrape && spec.Mode != ModeRTTScrape {
		return nil
	}
	if spec.Expect == "" {
		return fmt.Errorf("%w: %s declares %s, whose verdict is HIL_EXPECT, and declares no HIL_EXPECT",
			ErrInvalidManifest, spec.Path, spec.Mode)
	}
	if len(spec.Expect) < minimumExpectationBytes && !spec.Values["HIL_EXPECT_SHORT_OK"].Flag {
		return fmt.Errorf("%w: %s declares a %d-byte HIL_EXPECT under the %d-byte floor without HIL_EXPECT_SHORT_OK",
			ErrInvalidManifest, spec.Path, len(spec.Expect), minimumExpectationBytes)
	}
	return nil
}
