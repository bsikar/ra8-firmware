// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"fmt"
	"regexp"
	"regexp/syntax"
)

// checkExpectationIsNotAlsoForbidden refuses a text-capture manifest that
// forbids text its own positive expectation carries.
//
// The two expectations are read one after the other and they are read against
// the same bytes. VerifyTextCapture first requires HIL_EXPECT to appear in the
// capture as a literal byte substring, then requires HIL_EXPECT_NEGATIVE not
// to match anywhere in that same capture. So when the forbidden pattern is
// text the required expectation itself contains, the two cannot both hold:
// finding the expectation puts the forbidden text in the capture by the same
// act, and the attempt ends ErrNegativeExpectation no matter what the board
// did. The manifest asks for a board in order to fail.
//
// That is decidable from the manifest alone, and it is decided here for the
// same reason its two neighbours are: by the verdict the run has cost what a
// HIL run costs, the board leased, the fixture neutralized, the image flashed,
// the observation window spent whole, and firmware that may have been perfect
// is charged with a failure its manifest guaranteed.
//
// Only the alternatives that forbid a plain text on their own are judged. A
// pattern is a claim about the capture, not about the expectation, and most of
// them are conditional on text no expectation need carry: "lpm.*failed" spans
// bytes the expectation would have to supply, and "^HardFault" asks for a line
// start the expectation cannot promise, since the expectation may appear
// anywhere on a line. A plain literal asks for neither. It matches on the
// strength of its own bytes wherever they appear, so if the expectation
// carries them, every capture that satisfies the expectation carries them too.
// Anything less certain than that is left to the run, which is why an
// alternation is taken apart rather than matched whole: "FAIL|ok=N" forbids
// "FAIL" unconditionally even though the pattern as a whole is not a literal.
//
// Scoped to the two modes whose verdict is the text capture, the line
// expectation_before_the_board.go draws: under jlink_memprobe or hil_eth_tcp
// VerifyTextCapture never runs, neither expectation is read, and a
// contradiction between them costs nothing. Matching goes through the same
// helper and the same mode flags as the verdict, so the two doors cannot
// disagree about what the pattern forbids: under uart_scrape, which greps
// with -i, an expectation of "verdict=PASS" does carry a forbidden "FAIL"
// spelled "fail".
func checkExpectationIsNotAlsoForbidden(spec Spec) error {
	if spec.Mode != ModeUARTScrape && spec.Mode != ModeRTTScrape {
		return nil
	}
	if spec.Expect == "" || spec.ExpectNegative == "" {
		return nil
	}
	parsed, err := syntax.Parse(spec.ExpectNegative, syntax.Perl)
	if err != nil {
		// A pattern this build cannot apply is a different defect, and
		// checkNegativeExpectationCompiles has already refused it.
		return nil
	}
	for _, forbidden := range unconditionalLiterals(parsed) {
		matcher, err := compileNegativeExpectation(spec.Mode, forbidden.pattern)
		if err != nil || !matcher.MatchString(spec.Expect) {
			continue
		}
		return fmt.Errorf("%w: %s requires HIL_EXPECT %q and forbids %q, which that expectation carries",
			ErrInvalidManifest, spec.Path, spec.Expect, forbidden.text)
	}
	return nil
}

// forbiddenLiteral is one plain text a negative expectation forbids on its
// own, carried beside the pattern that matches exactly that text so the
// verdict's own compiler decides the containment.
type forbiddenLiteral struct {
	text    string
	pattern string
}

// unconditionalLiterals lists the texts this pattern forbids on the strength
// of their own bytes alone.
//
// A literal qualifies, an alternation contributes whichever of its branches
// qualify, and a capturing group is its contents. Everything else is
// conditional on the surrounding capture, so it is not judged here: a branch
// carrying a quantifier, a character class, or an anchor may or may not fire
// over a given capture, and only the run can say which.
//
// An empty branch is included deliberately. "FAIL|" forbids the empty string,
// which matches every capture ever taken, so such a manifest fails whatever
// the expectation is.
func unconditionalLiterals(parsed *syntax.Regexp) []forbiddenLiteral {
	switch parsed.Op {
	case syntax.OpLiteral:
		text := string(parsed.Rune)
		pattern := regexp.QuoteMeta(text)
		if parsed.Flags&syntax.FoldCase != 0 {
			pattern = "(?i)" + pattern
		}
		return []forbiddenLiteral{{text: text, pattern: pattern}}
	case syntax.OpEmptyMatch:
		return []forbiddenLiteral{{text: "", pattern: ""}}
	case syntax.OpCapture:
		return unconditionalLiterals(parsed.Sub[0])
	case syntax.OpAlternate:
		literals := make([]forbiddenLiteral, 0, len(parsed.Sub))
		for _, branch := range parsed.Sub {
			literals = append(literals, unconditionalLiterals(branch)...)
		}
		return literals
	default:
		return nil
	}
}
