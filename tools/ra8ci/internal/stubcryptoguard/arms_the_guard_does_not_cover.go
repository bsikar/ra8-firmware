// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package stubcryptoguard

import (
	"regexp"
	"strings"
)

// elifDirective matches a #elif. The preprocessor treats it as the end of the
// arm above it exactly the way #else does, so the guard's insecure region ends
// at whichever of the two comes first. ifDirective cannot stand in for it: its
// pattern anchors "if" straight after the hash, and "elif" does not start with
// one, so an #elif was previously invisible to the scan in both directions --
// it neither opened a nesting level nor closed the arm it actually closes.
var elifDirective = regexp.MustCompile(`^\s*#\s*elif\b`)

// opensAnArm reports whether the line ends the arm above it and opens another
// one at the same nesting level.
func opensAnArm(line string) bool {
	return elifDirective.MatchString(line) || elseDirective.MatchString(line)
}

// armStarts returns the directive line that opens each arm of the guard after
// the insecure one, from insecureEnd through the matching #endif at endIndex.
// insecureEnd is always the first entry; a chain of #elif arms adds one entry
// each, and a trailing #else adds the last.
//
// Every one of them matters. The gate only ever read the final arm, so an
// #elif arm compiled the placeholder crypto under some macro other than the two
// the guard names, returned k_ra8_ok, and the gate judged a branch further down
// the file instead. An arm that is not the insecure arm is a production arm and
// has to fail closed, whichever spelling opened it.
func armStarts(lines []string, insecureEnd, endIndex int) []int {
	if insecureEnd < 0 || endIndex <= insecureEnd || endIndex > len(lines) {
		return nil
	}
	starts := []int{insecureEnd}
	depth := 0
	for i := insecureEnd + 1; i < endIndex; i++ {
		switch {
		case ifDirective.MatchString(lines[i]):
			depth++
		case endifDirective.MatchString(lines[i]):
			if depth > 0 {
				depth--
			}
		case depth == 0 && opensAnArm(lines[i]):
			starts = append(starts, i)
		}
	}
	return starts
}

// armFailsClosed reports whether the arm's body refuses to build or returns a
// hard error. A #error stops the translation unit; a k_ra8_err_* return hands
// the refusal to the caller. Anything else, k_ra8_ok included, means a
// production build silently got the placeholder's answer.
func armFailsClosed(body []string) bool {
	for _, line := range body {
		if errorDirective.MatchString(line) || strings.Contains(line, "k_ra8_err_") {
			return true
		}
	}
	return false
}
