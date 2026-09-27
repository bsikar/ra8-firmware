// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"strings"
	"unicode"
)

// declared_spelling.go and declared_value.go both hold the same line: this
// reader and the shell that sources the same file have to agree about
// whether the app declared a timeout at all. Both of them judge the NAME.
// Neither of them judges whether what carries the name is an assignment.
//
// It is whitespace that decides that, and the shell is strict about it. A
// name, then "=", with nothing between them, is an assignment. Anything else
// is not:
//
//	HIL_TIMEOUT_S =180
//
// is the command HIL_TIMEOUT_S run with the argument "=180", which sets
// nothing and fails with "command not found", and
//
//	HIL_TIMEOUT_S= 180
//
// assigns the EMPTY string and then runs 180 as a command, so the bench
// runner's HIL_TIMEOUT_S:-30 falls through to the default because an empty
// value is what :- exists to catch. Either way the shell leaves the bench on
// 30s.
//
// This reader trimmed both sides before comparing, so it read 180 from both
// lines and reported a declaration the shell never made. That is the
// disagreement the other two doors exist to prevent, arriving through the one
// line shape they both pass: the key spells the name exactly, so the spelling
// door is satisfied, and the line carries an "=", so the value door is not
// asked. An app whose observe step needs three minutes then runs under 30s
// and is reported timed out, while the control plane's own audit record says
// the bound came from hil.conf and was 180.
//
// So a line that names HIL_TIMEOUT_S around an "=" a shell would not read as
// an assignment is a declaration this reader cannot read, and it is refused
// with the line named rather than quietly taken, which is how every other
// refusal in this reader already behaves. The boundary is the one the other
// doors draw: this is asked only once the key spells HIL_TIMEOUT_S and
// nothing else, so HIL_MODE = uart_scrape and RA8_HIL_TIMEOUT_S = 180 are
// still skipped in silence, and an unread spelling is still reported as an
// unread spelling rather than as spacing.

// shellAssignsHere reports whether key and value, as they were written either
// side of the first "=" of a hil.conf line, form an assignment a shell
// sourcing that line would make.
//
// The line has already been trimmed, so leading space on key and trailing
// space on value are not this rule's business. What is left is the two
// positions the shell reads: the byte before the "=" and the byte after it.
func shellAssignsHere(key, value string) bool {
	if key == "" {
		return false
	}
	if strings.TrimRightFunc(key, unicode.IsSpace) != key {
		return false
	}
	return strings.TrimLeftFunc(value, unicode.IsSpace) == value
}
