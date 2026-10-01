// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gotosetjmp

import (
	"strings"
	"testing"
)

// tokensFound reports the banned tokens the scan reported, in order, so a case
// can assert both that a token was found and that nothing else was.
func tokensFound(text string) []string {
	var tokens []string
	for _, item := range scanText(text) {
		tokens = append(tokens, item.token)
	}
	return tokens
}

// A character literal is a quote that does not open a string. Nothing in the
// scan entered one before, so a file holding a double quote inside single
// quotes would have flipped the scan into string state and stayed there,
// hiding every banned token for the rest of the file. That is the failure
// direction that matters: this gate exists to find goto, and a scan that
// silently stops looking reports a clean tree.
func TestAQuoteInsideACharacterLiteralDoesNotOpenAString(t *testing.T) {
	for name, text := range map[string]string{
		"double quote in a character literal": "char c = '\"'; goto done;\n",
		"escaped single quote":                "char q = '\\''; goto done;\n",
		"escaped backslash":                   "char b = '\\\\'; goto done;\n",
	} {
		if got := tokensFound(text); len(got) != 1 || got[0] != "goto" {
			t.Errorf("%s: reported %v, want the goto after the literal", name, got)
		}
	}
}

// A literal that never closes ends at the newline. Without that rule a single
// stray quote anywhere in a source file would swallow everything below it and
// the gate would call the file clean, which is the one answer it must never
// get wrong.
func TestAnUnterminatedLiteralEndsAtTheNewline(t *testing.T) {
	for name, text := range map[string]string{
		"unterminated string":    "char *s = \"oops\ngoto done;\n",
		"unterminated character": "char c = 'x\ngoto done;\n",
	} {
		got := tokensFound(text)
		if len(got) != 1 || got[0] != "goto" {
			t.Errorf("%s: reported %v, want the goto on the following line", name, got)
		}
	}
}

// The one case where a literal does cross the newline is an explicit backslash
// continuation, and there the token really is inside the string. Kept beside
// the case above because the two rules are one line apart in the scan and a
// change that dropped the distinction would otherwise pass.
func TestABackslashContinuedStringKeepsSwallowingTheNextLine(t *testing.T) {
	if got := tokensFound("char *s = \"abc\\\ngoto done;\";\n"); len(got) != 0 {
		t.Fatalf("reported %v, want nothing: the token is inside a continued string", got)
	}
	// The same shape for a character literal, which is the arm that had no
	// coverage at all.
	if got := tokensFound("char c = 'a\\\ngoto done;';\n"); len(got) != 0 {
		t.Fatalf("reported %v, want nothing: the token is inside a continued literal", got)
	}
}

// A banned token still has to be found on the line after a properly closed
// literal, so the cases above are not passing merely because the scan gave up.
func TestAClosedLiteralLeavesTheScanLooking(t *testing.T) {
	text := "char c = 'x';\nchar *s = \"goto\";\ngoto real;\nsetjmp(buf);\n"
	got := tokensFound(text)
	if strings.Join(got, ",") != "goto,setjmp" {
		t.Fatalf("reported %v, want the goto and setjmp outside the literals", got)
	}
}
