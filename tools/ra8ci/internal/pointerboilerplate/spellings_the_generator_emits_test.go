// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package pointerboilerplate

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

// flagged is the question the scan asks of every line.
func flagged(line string) bool { return lineIsGeneratedPointerComment(line) }

// scanLines runs the real scan over a temp tree holding one source file of
// these lines and returns the line numbers it reported.
func scanLines(t *testing.T, lines ...string) []int {
	t.Helper()
	root := t.TempDir()
	rel := "apps/demo/src/demo.c"
	if err := os.MkdirAll(filepath.Join(root, filepath.Dir(rel)), 0o750); err != nil {
		t.Fatalf("mkdir fixture: %v", err)
	}
	body := ""
	for _, line := range lines {
		body += line + "\n"
	}
	if err := os.WriteFile(filepath.Join(root, filepath.FromSlash(rel)), []byte(body), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}
	found, err := scan(context.Background(), root, []string{rel})
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	numbers := make([]int, 0, len(found))
	for _, item := range found {
		numbers = append(numbers, item.line)
	}
	return numbers
}

func TestThePublicHeaderSpellingIsRejected(t *testing.T) {
	if !flagged("/* See the public header for the documented contract. */") {
		t.Fatal("the spelling this generator emits most often must be rejected")
	}
}

func TestTheInternalAndBareHeaderSpellingsStayRejected(t *testing.T) {
	for _, line := range []string{
		"/* See the internal header for the documented contract. */",
		"/* see header for the documented contract. */",
		"/* See the private header for the documented contract. */",
	} {
		if !flagged(line) {
			t.Errorf("still a generated comment: %q", line)
		}
	}
}

func TestTheLineCommentSpellingIsRejected(t *testing.T) {
	for _, line := range []string{
		"// See the public header for the documented contract.",
		"//See the internal header for the documented contract.",
		"    // see header for the documented contract.",
	} {
		if !flagged(line) {
			t.Errorf("a line comment carrying the same sentence is the same comment: %q", line)
		}
	}
}

func TestIndentationAndTrailingSpaceDoNotMatter(t *testing.T) {
	if !flagged("\t  /* See the public header for the documented contract. */   ") {
		t.Fatal("leading indent and trailing space must not hide the comment")
	}
}

func TestLegacyWordingIsNotRejected(t *testing.T) {
	for _, line := range []string{
		"/* see header for full description */",
		"// see header for full description",
	} {
		if flagged(line) {
			t.Errorf("legacy wording is not this comment: %q", line)
		}
	}
}

func TestAnImplementationNoteIsNotRejected(t *testing.T) {
	for _, line := range []string{
		"/* See header for the documented contract -- bounded scan. */",
		"// See the public header for the documented contract; the retry is local.",
	} {
		if flagged(line) {
			t.Errorf("a comment saying something of its own must survive: %q", line)
		}
	}
}

func TestAStringLiteralIsNotRejected(t *testing.T) {
	for _, line := range []string{
		"const char* text = \"see header for the documented contract.\";",
		"puts(\"// see header for the documented contract.\");",
	} {
		if flagged(line) {
			t.Errorf("a string literal is not a comment: %q", line)
		}
	}
}

func TestACommentSharingItsLineWithCodeIsNotRejected(t *testing.T) {
	if flagged("int x = 1; /* See the public header for the documented contract. */") {
		t.Fatal("the gate rejects a comment that is the whole line, not a trailing one")
	}
}

func TestAnUnterminatedBlockCommentIsNotRejected(t *testing.T) {
	if flagged("/* See the public header for the documented contract.") {
		t.Fatal("an unterminated block comment opens something larger than this sentence")
	}
}

func TestScanReportsEverySpellingInASourceFile(t *testing.T) {
	numbers := scanLines(t,
		"#include \"demo.h\"",
		"/* See the public header for the documented contract. */",
		"void demo_start(void) { }",
		"// See the internal header for the documented contract.",
		"/* See header for the documented contract -- bounded scan. */",
		"void demo_stop(void) { }",
	)
	if len(numbers) != 2 || numbers[0] != 2 || numbers[1] != 4 {
		t.Fatalf("scan reported lines %v, want [2 4]", numbers)
	}
}
