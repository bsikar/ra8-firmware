// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"bytes"
	"fmt"
	"regexp"
)

// negativeExpectationMatches reports whether HIL_EXPECT_NEGATIVE matches the
// captured bytes, judging them the way the manifest's contract says they are
// judged.
//
// That contract is grep's: scripts/hil run the negative expectation through
// grep -iE (UART) or grep -qE (RTT), and grep applies a pattern to one LINE at
// a time. So "^" and "$" in a manifest are line anchors, and a capture is many
// lines. RE2 without the m flag anchors them to the ends of the whole text,
// and Go's "$" does not even match before a trailing newline, so
// "^HardFault" or "verdict=FAIL$" written against the contract matched nothing
// here however many lines carried it. A negative expectation that cannot fire
// is worse than none: the manifest says a banner must not appear, the run
// carries it on its own line, and the capture verifies clean.
func negativeExpectationMatches(mode Mode, pattern string, captured []byte) (bool, error) {
	compiled, err := compileNegativeExpectation(mode, pattern)
	if err != nil {
		return false, err
	}
	// grep reads lines, and a line's terminator is not part of it, so the
	// newline that ends the last line does not open an empty line after it.
	// The m flag alone would treat the position past it as one, and "^$" or
	// any other pattern that can match empty would fire on a capture no line
	// of which is empty. Trimming exactly one terminator, never the \r before
	// it (grep keeps that in the line), leaves every real line intact.
	return compiled.Match(bytes.TrimSuffix(captured, []byte("\n"))), nil
}

// compileNegativeExpectation wraps the manifest's pattern in the flags its
// mode greps with.
//
// The m flag restores the line anchoring described above and nothing else. The
// s flag is deliberately NOT set: "." not crossing a newline is what grep does
// too, and a dot allowed to span lines would let a negative expectation match
// text no single line ever held.
func compileNegativeExpectation(mode Mode, pattern string) (*regexp.Regexp, error) {
	flags := "m"
	if mode == ModeUARTScrape {
		// UART greps with -i. RTT does not; preserve that distinction.
		flags = "im"
	}
	// The inner group keeps a top-level alternation inside the flag group, so
	// "a|b" stays two alternatives of the whole pattern rather than binding
	// across the wrapper.
	compiled, err := regexp.Compile("(?" + flags + ":(?:" + pattern + "))")
	if err != nil {
		return nil, fmt.Errorf("invalid HIL negative expectation: %w", err)
	}
	return compiled, nil
}
