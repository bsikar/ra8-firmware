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
	for _, args := range [][]string{{}, {"--all", "path"}, {"--checkout"}, {"--all", "--checkout", "x.md"}, {"--unknown"}, {"missing.md"}} {
		code := Run(context.Background(), root, args, io.Discard, io.Discard)
		if code != 2 {
			t.Fatalf("Run(%q) = %d, want 2", args, code)
		}
	}
}

func TestRunCheckoutRewriteConfinesTargetToCheckout(t *testing.T) {
	root := t.TempDir()
	inside := filepath.Join(root, "docs", "target.md")
	if err := os.MkdirAll(filepath.Dir(inside), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(inside, []byte("dash—\n"), 0600); err != nil {
		t.Fatal(err)
	}
	outside := filepath.Join(t.TempDir(), "outside.md")
	if err := os.WriteFile(outside, []byte("outside—\n"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, target := range []string{"../outside.md", outside} {
		if code := Run(context.Background(), root, []string{"--checkout", target}, io.Discard, io.Discard); code != 2 {
			t.Fatalf("Run checkout target %q = %d, want 2", target, code)
		}
	}
	if code := Run(context.Background(), root, []string{"--checkout", "docs/target.md"}, io.Discard, io.Discard); code != 0 {
		t.Fatalf("Run checkout rewrite = %d, want 0", code)
	}
	contents, err := os.ReadFile(inside)
	if err != nil || string(contents) != "dash--\n" {
		t.Fatalf("rewritten checkout file = %q, %v", contents, err)
	}
	contents, err = os.ReadFile(outside)
	if err != nil || string(contents) != "outside—\n" {
		t.Fatalf("outside file changed: %q, %v", contents, err)
	}
}

func TestRunCheckoutRefusesSymlinkComponents(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	if err := os.WriteFile(filepath.Join(outside, "target.md"), []byte("dash—\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(root, "linked")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	if code := Run(context.Background(), root, []string{"--checkout", "linked/target.md"}, io.Discard, io.Discard); code != 2 {
		t.Fatalf("Run through symlink = %d, want 2", code)
	}
}
