// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import "fmt"

// checkNegativeExpectationCompiles refuses a manifest whose
// HIL_EXPECT_NEGATIVE is not a pattern this build can apply.
//
// Parse establishes every other typed value the moment it reads it: the mode
// against the six it supports, HIL_BOARD_IP as a literal address, HIL_PROTO
// against tcp or udp, every probe symbol against an identifier. The negative
// expectation was the one field whose validity nothing established. It is a
// regular expression, and whether a string is one is only discoverable by
// compiling it, which happened for the first time inside VerifyTextCapture,
// at the verdict.
//
// By then the run has cost what a HIL run costs: the board leased, the
// fixture neutralized, the image flashed, the whole observation window spent,
// the capture taken. A bracket left open in a manifest then surfaces as an
// error about the manifest, charged to an attempt that had nothing wrong with
// its firmware, and the attempt carries a parse failure where its verdict
// should be. Every other door here fails closed before a board is reserved;
// this one failed open until the board was finished with.
//
// It is a malformed value, not an unused one, so it is refused under every
// mode including the modes that never read it, exactly as a HIL_BOARD_IP that
// is not an address is refused under modes that never open a socket. Which
// knobs a mode ignores is a different rule and not this one.
//
// The pattern is compiled through the same helper the verdict uses, under the
// same mode-dependent flags, so the two doors cannot disagree about what a
// pattern is: anything accepted here, VerifyTextCapture can apply.
func checkNegativeExpectationCompiles(spec Spec) error {
	if spec.ExpectNegative == "" {
		return nil
	}
	if _, err := compileNegativeExpectation(spec.Mode, spec.ExpectNegative); err != nil {
		return fmt.Errorf("%w: %s: %v", ErrInvalidManifest, spec.Path, err)
	}
	return nil
}
