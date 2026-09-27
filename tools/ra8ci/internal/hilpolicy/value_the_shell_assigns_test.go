// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestPlainValueIsWhatTheShellAssigns(t *testing.T) {
	value, readable := valueTheShellAssigns("180")
	if !readable || value != "180" {
		t.Fatalf("valueTheShellAssigns(180) = %q, %v", value, readable)
	}
}

func TestTrailingBlanksAreNotPartOfTheValue(t *testing.T) {
	value, readable := valueTheShellAssigns("180\t ")
	if !readable || value != "180" {
		t.Fatalf("trailing blanks = %q, %v", value, readable)
	}
}

func TestInlineCommentIsDiscarded(t *testing.T) {
	for _, raw := range []string{"180 # three minutes", "180\t#three minutes", "180 #"} {
		value, readable := valueTheShellAssigns(raw)
		if !readable || value != "180" {
			t.Fatalf("valueTheShellAssigns(%q) = %q, %v", raw, value, readable)
		}
	}
}

func TestHashInsideTheWordIsLiteral(t *testing.T) {
	value, readable := valueTheShellAssigns("180#x")
	if !readable || value != "180#x" {
		t.Fatalf("mid-word hash = %q, %v", value, readable)
	}
}

func TestQuotedValueIsUnquoted(t *testing.T) {
	for _, raw := range []string{`"180"`, `'180'`, `"180" # three minutes`} {
		value, readable := valueTheShellAssigns(raw)
		if !readable || value != "180" {
			t.Fatalf("valueTheShellAssigns(%q) = %q, %v", raw, value, readable)
		}
	}
}

func TestAdjacentQuotedRunsJoin(t *testing.T) {
	value, readable := valueTheShellAssigns(`'18'"0"`)
	if !readable || value != "180" {
		t.Fatalf("joined runs = %q, %v", value, readable)
	}
}

func TestQuotedBlankStaysInTheWord(t *testing.T) {
	value, readable := valueTheShellAssigns(`"180 # x"`)
	if !readable || value != "180 # x" {
		t.Fatalf("quoted blank = %q, %v", value, readable)
	}
}

func TestUnterminatedQuoteIsUnreadable(t *testing.T) {
	for _, raw := range []string{`"180`, `'180`, `"180' `} {
		if _, readable := valueTheShellAssigns(raw); readable {
			t.Fatalf("valueTheShellAssigns(%q) read an unterminated quote", raw)
		}
	}
}

func TestExpansionIsUnreadable(t *testing.T) {
	for _, raw := range []string{"$TIMEOUT", "`echo 180`", `18\0`, `"$TIMEOUT"`} {
		if _, readable := valueTheShellAssigns(raw); readable {
			t.Fatalf("valueTheShellAssigns(%q) read an expansion", raw)
		}
	}
}

func TestSecondWordIsUnreadable(t *testing.T) {
	for _, raw := range []string{"180 run_bench", `"180" run_bench`, "180 180"} {
		if _, readable := valueTheShellAssigns(raw); readable {
			t.Fatalf("valueTheShellAssigns(%q) read a second word as an assignment", raw)
		}
	}
}

func TestEmptyAssignmentResolvesToEmpty(t *testing.T) {
	value, readable := valueTheShellAssigns("")
	if !readable || value != "" {
		t.Fatalf("empty assignment = %q, %v", value, readable)
	}
}

// The reader end: a commented and a quoted declaration are read, and what
// stays unreadable is named as an unreadable declaration rather than reported
// as an invalid value.

func writeShellValueConfig(t *testing.T, line string) string {
	t.Helper()
	root := t.TempDir()
	app := filepath.Join(root, "examples", "ek_ra8d2", "hw_validated", "hil", "shellvalue")
	if err := os.MkdirAll(app, 0o755); err != nil {
		t.Fatalf("make app directory: %v", err)
	}
	if err := os.WriteFile(filepath.Join(app, "hil.conf"), []byte(line+"\n"), 0o644); err != nil {
		t.Fatalf("write hil.conf: %v", err)
	}
	return root
}

func TestDeclaredTimeoutReadsACommentedDeclaration(t *testing.T) {
	root := writeShellValueConfig(t, "HIL_TIMEOUT_S=180 # three minutes")
	seconds, found, err := DeclaredTimeout(root, "shellvalue")
	if err != nil || !found || seconds != 180 {
		t.Fatalf("DeclaredTimeout = %d, %v, %v", seconds, found, err)
	}
}

func TestDeclaredTimeoutReadsAQuotedDeclaration(t *testing.T) {
	root := writeShellValueConfig(t, `HIL_TIMEOUT_S="180"`)
	seconds, found, err := DeclaredTimeout(root, "shellvalue")
	if err != nil || !found || seconds != 180 {
		t.Fatalf("DeclaredTimeout = %d, %v, %v", seconds, found, err)
	}
}

func TestDeclaredTimeoutRefusesAnUnreadableValue(t *testing.T) {
	for _, line := range []string{
		`HIL_TIMEOUT_S="180`,
		"HIL_TIMEOUT_S=$TIMEOUT",
		"HIL_TIMEOUT_S=180 run_bench",
	} {
		root := writeShellValueConfig(t, line)
		_, found, err := DeclaredTimeout(root, "shellvalue")
		if found {
			t.Fatalf("DeclaredTimeout(%q) reported a declaration", line)
		}
		if !errors.Is(err, ErrUnreadableDeclaration) {
			t.Fatalf("DeclaredTimeout(%q) error = %v, want ErrUnreadableDeclaration", line, err)
		}
	}
}

func TestDeclaredTimeoutStillRefusesAResolvedNonNumber(t *testing.T) {
	root := writeShellValueConfig(t, `HIL_TIMEOUT_S="soon"`)
	_, found, err := DeclaredTimeout(root, "shellvalue")
	if found || err == nil {
		t.Fatalf("DeclaredTimeout = %v, %v", found, err)
	}
	if errors.Is(err, ErrUnreadableDeclaration) {
		t.Fatalf("a resolved non-number was reported as unreadable: %v", err)
	}
}

func TestDeclaredTimeoutStillRefusesAnOutOfBoundsQuotedValue(t *testing.T) {
	root := writeShellValueConfig(t, `HIL_TIMEOUT_S="0" # none`)
	_, found, err := DeclaredTimeout(root, "shellvalue")
	if found || err == nil {
		t.Fatalf("DeclaredTimeout = %v, %v", found, err)
	}
}
