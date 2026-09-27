// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import "strings"

// definesVeneer reports whether source carries a real definition of name, not
// merely another declaration of it.
//
// The gate exists because a veneer declared to the non-secure world with no
// implementation behind it is a phantom entry point. Matching the annotated
// name anywhere in a .c file does not establish that: a forward declaration
// copied into the source, which ends at a semicolon and emits nothing, read as
// proof of a definition and closed the finding. So did the same text sitting
// inside a comment. Either one silences the gate on exactly the hazard it was
// written to catch.
//
// A definition is the parameter list closing and a body opening after it, so
// the match is followed to its matching parenthesis and the next meaningful
// character has to be '{'. Whitespace and comments between the two are
// ordinary C and are skipped; anything else, a semicolon above all, means this
// occurrence was a declaration and the scan moves on to the next one.
func definesVeneer(name string, source []byte) bool {
	pattern := definitionPattern(name)
	text := string(source)
	for searched := 0; searched < len(text); {
		match := pattern.FindStringIndex(text[searched:])
		if match == nil {
			return false
		}
		openParen := searched + match[1] - 1
		searched += match[1]
		closeParen := matchingParen(text, openParen)
		if closeParen < 0 {
			continue
		}
		if bodyOpensAt(text, closeParen+1) {
			return true
		}
	}
	return false
}

// matchingParen returns the index of the parenthesis closing the one at open,
// or -1 when the file ends first. Parameter lists nest (a function pointer
// parameter carries its own), so depth is counted rather than assumed.
func matchingParen(text string, open int) int {
	depth := 0
	for index := open; index < len(text); index++ {
		switch text[index] {
		case '(':
			depth++
		case ')':
			depth--
			if depth == 0 {
				return index
			}
		}
	}
	return -1
}

// bodyOpensAt reports whether the first meaningful character from index is the
// brace of a function body, skipping whitespace and both comment forms.
func bodyOpensAt(text string, index int) bool {
	for index < len(text) {
		switch {
		case text[index] == ' ', text[index] == '\t', text[index] == '\r', text[index] == '\n':
			index++
		case strings.HasPrefix(text[index:], "//"):
			end := strings.IndexByte(text[index:], '\n')
			if end < 0 {
				return false
			}
			index += end + 1
		case strings.HasPrefix(text[index:], "/*"):
			end := strings.Index(text[index+2:], "*/")
			if end < 0 {
				return false
			}
			index += 2 + end + 2
		default:
			return text[index] == '{'
		}
	}
	return false
}
