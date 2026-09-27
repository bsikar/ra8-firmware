// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import "strings"

// The three doors before this one all hold the same line: this reader and the
// shell that sources the same file have to agree about whether the app
// declared a timeout at all. They judge the NAME, and the one before this one
// judges whether the "=" is an assignment. None of them judges the VALUE, and
// the value is where the same disagreement comes back.
//
// Once the key spells HIL_TIMEOUT_S and the "=" assigns, this reader handed
// everything after the "=" to strconv.Atoi with nothing but a trim. A shell
// hands that text to its own word splitter first, and two ordinary things a
// config meant for sourcing does are invisible to Atoi:
//
//	HIL_TIMEOUT_S=180 # three minutes
//	HIL_TIMEOUT_S="180"
//
// The shell assigns 180 in both. Atoi sees "180 # three minutes" and `"180"`,
// fails on each, and DeclaredTimeout returns `invalid HIL_TIMEOUT_S`. So an
// app whose configuration the bench reads perfectly cannot be read here at
// all, and the error names the wrong thing: the value is 180 and is not
// invalid. That refusal is the mirror of the failure the other doors catch.
// They catch this reader accepting a declaration the shell never made; this
// one catches it refusing a declaration the shell makes cleanly. Both end the
// same way, with the two readers disagreeing about the declared bound.
//
// What is asked here is what the shell would assign, which is one word:
// everything from after the "=" up to the first unquoted blank, with one
// matched quote pair per run removed, and an unquoted "#" that begins a later
// word discarded as the comment it is. Note the boundary the shell itself
// draws and this follows: a "#" is a comment only at the start of a word, so
// HIL_TIMEOUT_S=180#x assigns the literal "180#x" and is not a comment.
//
// Everything this reader cannot resolve without evaluating shell syntax fails
// closed as an unreadable declaration rather than being guessed at, which is
// how every other refusal here already behaves: an unterminated quote, a "$"
// or backtick or backslash this reader deliberately does not expand, and a
// second word after the value, which the shell reads as a COMMAND run with
// HIL_TIMEOUT_S in its environment and not as an assignment the sourcing
// shell keeps at all. A value that resolves and is still not a number in
// bounds is not this rule's business: it stays the invalid value it always
// was.

// valueTheShellAssigns reports the text a shell sourcing the line would
// assign, given the raw text written after the first "=" of a hil.conf line
// whose key spells HIL_TIMEOUT_S and whose "=" already assigns.
//
// readable is false when resolving it would take evaluating shell syntax this
// reader does not evaluate, which is a declaration to refuse and name, not a
// value to guess.
func valueTheShellAssigns(raw string) (value string, readable bool) {
	var assigned strings.Builder
	var quote byte
	index := 0
	for index < len(raw) {
		character := raw[index]
		switch {
		case quote == 0 && isBlank(character):
			// The word, and the assignment with it, ends here.
			return afterTheWord(assigned.String(), raw[index:])
		case character == '\\' || character == '$' || character == '`':
			return "", false
		case quote == 0 && (character == '\'' || character == '"'):
			quote = character
		case quote != 0 && character == quote:
			quote = 0
		default:
			assigned.WriteByte(character)
		}
		index++
	}
	if quote != 0 {
		return "", false
	}
	return assigned.String(), true
}

// afterTheWord judges what follows the assigned word. Nothing but blanks is
// the ordinary case, and a "#" beginning the next word is a comment the shell
// discards. Anything else is a second word, which is a different statement
// from an assignment.
func afterTheWord(assigned, rest string) (string, bool) {
	trimmed := strings.TrimLeft(rest, " \t")
	if trimmed == "" || strings.HasPrefix(trimmed, "#") {
		return assigned, true
	}
	return "", false
}

// isBlank reports whether b separates words on a hil.conf line. The line was
// read by a scanner splitting on newlines and then trimmed, so a space and a
// tab are the two that can still be here.
func isBlank(b byte) bool {
	return b == ' ' || b == '\t'
}
