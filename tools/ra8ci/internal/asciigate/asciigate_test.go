// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func TestTransliterateAndNewlinePolicy(t *testing.T) {
	input := "— – ‘ ’ “ ” …  ° ± µ μ ≤ ≥ ≠ → ← × ÷ é"
	want := "-- - ' ' \" \" ...   deg +/- u u <= >= != -> <- x / ?"
	got, count := transliterate(input)
	if got != want || count != 20 {
		t.Fatalf("transliterate = %q, %d; want %q, 20", got, count, want)
	}
	if got := normalizeNewlines("a\r\nb\rc"); got != "a\nb\nc" {
		t.Fatalf("newline normalization = %q", got)
	}
}

func TestProcessCheckAndRewrite(t *testing.T) {
	path := filepath.Join(t.TempDir(), "text.md")
	if err := os.WriteFile(path, []byte("dash—\r\n"), 0600); err != nil {
		t.Fatal(err)
	}
	count, err := process(path, false)
	if err != nil || count != 1 {
		t.Fatalf("check process = %d, %v; want 1, nil", count, err)
	}
	contents, err := os.ReadFile(path)
	if err != nil || string(contents) != "dash—\r\n" {
		t.Fatalf("check modified file: %q, %v", contents, err)
	}
	count, err = process(path, true)
	if err != nil || count != 1 {
		t.Fatalf("rewrite process = %d, %v; want 1, nil", count, err)
	}
	contents, err = os.ReadFile(path)
	if err != nil || string(contents) != "dash--\n" {
		t.Fatalf("rewrite contents = %q, %v", contents, err)
	}
}

func TestRunRequiresModeAndRejectsMissingTarget(t *testing.T) {
	root := t.TempDir()
	for _, args := range [][]string{{}, {"--all", "path"}, {"--unknown"}, {"missing.md"}} {
		code := Run(context.Background(), root, args, io.Discard, io.Discard)
		if code != 2 {
			t.Fatalf("Run(%q) = %d, want 2", args, code)
		}
	}
}
