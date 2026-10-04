// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/privatefile"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

// What the gate actually reports once it has a scope: the verdict lines a
// reviewer reads, and the refusals that keep an untrustworthy scan from
// looking like a clean one.

func decimal(value int) string {
	if value == 0 {
		return "0"
	}
	var digits []byte
	for value > 0 {
		digits = append([]byte{byte('0' + value%10)}, digits...)
		value /= 10
	}
	return string(digits)
}

func plantFullScope(t *testing.T, extra map[string]string) string {
	t.Helper()
	files := make(map[string]string, fileFloor+len(extra))
	for index := 0; index < fileFloor; index++ {
		files["apps/unit"+decimal(index)+".c"] = "/* unit */\n"
	}
	for rel, body := range extra {
		files[rel] = body
	}
	return plantRepo(t, files)
}

func ranGate(t *testing.T, root string, args ...string) (int, string, string) {
	t.Helper()
	var out, errs bytes.Buffer
	code := Run(context.Background(), root, args, &out, &errs)
	return code, out.String(), errs.String()
}

// A full derived scan with nothing to fix reports the totals, which is the
// line that tells a reviewer the scan was real rather than collapsed.
func TestAFullDerivedScanReportsItsTotals(t *testing.T) {
	code, out, errs := ranGate(t, plantFullScope(t, nil), "--check", "--all")
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "ra8ci ascii: 0 non-ASCII character(s) across "+decimal(fileFloor)+" file(s)") {
		t.Fatalf("stdout = %q, want the totals over %d files", out, fileFloor)
	}
	if strings.Contains(out, "[NEEDS-FIX]") {
		t.Fatalf("stdout = %q, nothing needed fixing", out)
	}
}

// --check names each offending file with its count and fails, and it leaves
// the file alone: a check that quietly rewrote the tree would turn a
// reporting run into an edit nobody asked for.
func TestACheckedScanNamesEachFileAndChangesNothing(t *testing.T) {
	root := plantFullScope(t, map[string]string{"docs/notes.md": "the value is 5 \u00b0C \u2014 measured\n"})
	before, err := os.ReadFile(filepath.Join(root, "docs", "notes.md"))
	if err != nil {
		t.Fatalf("read the fixture: %v", err)
	}
	code, out, errs := ranGate(t, root, "--check", "--all")
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "[NEEDS-FIX] docs/notes.md: 2 non-ASCII characters") {
		t.Fatalf("stdout = %q, want the file and its count", out)
	}
	if !strings.Contains(out, "ra8ci ascii: 2 non-ASCII character(s) across") {
		t.Fatalf("stdout = %q, want the totals", out)
	}
	after, err := os.ReadFile(filepath.Join(root, "docs", "notes.md"))
	if err != nil {
		t.Fatalf("read it back: %v", err)
	}
	if string(after) != string(before) {
		t.Fatalf("the file changed under --check: %q", after)
	}
}

// Without --check the same scan rewrites, reports what it fixed, and exits
// 0: a rewrite run that fixed everything has nothing left to fail about.
func TestAnUncheckedScanRewritesAndSaysWhatItFixed(t *testing.T) {
	root := plantFullScope(t, map[string]string{"docs/notes.md": "the value is 5 \u00b0C \u2014 measured\n"})
	code, out, errs := ranGate(t, root, "--all")
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "[FIXED] docs/notes.md: 2 replacements") {
		t.Fatalf("stdout = %q, want the rewrite named", out)
	}
	fixed, err := os.ReadFile(filepath.Join(root, "docs", "notes.md"))
	if err != nil {
		t.Fatalf("read it back: %v", err)
	}
	if string(fixed) != "the value is 5  degC -- measured\n" {
		t.Fatalf("rewritten file = %q", fixed)
	}
}

// A file the scan cannot decode is a refusal naming the file, never a pass.
// An ASCII gate that skipped undecodable bytes would report a tree clean
// that it never read.
func TestAFileThatIsNotUTF8IsRefusedNotSkipped(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "blob.md")
	if err := os.WriteFile(path, []byte{0xff, 0xfe, 'a', '\n'}, 0o644); err != nil {
		t.Fatalf("plant the blob: %v", err)
	}
	code, out, errs := ranGate(t, root, "--check", path)
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, "invalid UTF-8; refusing a trusted ASCII scan") {
		t.Fatalf("stderr = %q, want the refusal", errs)
	}
}

// The same refusal holds through the confined checkout path, which reads
// the file through an opened root rather than by name.
func TestACheckoutTargetThatIsNotUTF8IsRefused(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "blob.md"), []byte{0xff, 0xfe, 'a', '\n'}, 0o644); err != nil {
		t.Fatalf("plant the blob: %v", err)
	}
	code, _, errs := ranGate(t, root, "--check", "--checkout", "blob.md")
	if code != 2 {
		t.Fatalf("code = %d, want 2", code)
	}
	if !strings.Contains(errs, "checkout target has invalid UTF-8") {
		t.Fatalf("stderr = %q, want the checkout refusal", errs)
	}
}

// A checkout rewrite keeps the file's own mode. Widening it would hand a
// tightened script back to the tree more readable than it was left.
func TestACheckoutRewriteKeepsTheFileMode(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "tight.md")
	if err := os.WriteFile(path, []byte("5 \u00b0C\n"), 0o600); err != nil {
		t.Fatalf("plant: %v", err)
	}
	if err := testprivatefile.OwnerOnly(path); err != nil {
		t.Fatalf("protect fixture: %v", err)
	}
	code, out, errs := ranGate(t, root, "--checkout", "tight.md")
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "[FIXED] tight.md: 1 replacements") {
		t.Fatalf("stdout = %q, want the rewrite named", out)
	}
	if err := privatefile.Check(path); err != nil {
		t.Fatalf("rewrite widened file access: %v", err)
	}
	if runtime.GOOS != "windows" {
		info, err := os.Stat(path)
		if err != nil {
			t.Fatalf("stat rewritten file: %v", err)
		}
		if info.Mode().Perm() != 0o600 {
			t.Fatalf("mode = %v, %v; want 0600 kept", info.Mode().Perm(), err)
		}
	}
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if string(body) != "5  degC\n" {
		t.Fatalf("rewritten = %q", body)
	}
}

// A target that is neither a regular file nor a directory is refused rather
// than walked: the walk has nothing to say about a device or a socket.
func TestATargetThatIsNeitherFileNorDirectoryIsRefused(t *testing.T) {
	code, out, errs := ranGate(t, t.TempDir(), "--check", os.DevNull)
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, "is not a regular file or directory") {
		t.Fatalf("stderr = %q, want the refusal", errs)
	}
}

// An excluded directory hides its files at ANY depth, not only at the top
// of the walked subtree, which is what keeps a vendored tree out of a
// subtree scan the way it stays out of the derived one.
func TestAnExcludedDirectoryHidesItsFilesAtAnyDepth(t *testing.T) {
	root := t.TempDir()
	writeFileAt(t, filepath.Join(root, "docs"), "kept.md", "plain\n")
	writeFileAt(t, filepath.Join(root, "docs", "deep", "third_party"), "vendor.md", "5 \u00b0C\n")
	writeFileAt(t, filepath.Join(root, "docs", "deep", "fixtures"), "sample.md", "5 \u00b0C\n")
	held := walked(t, root)
	if len(held) != 1 || !held["docs/kept.md"] {
		t.Fatalf("walk = %v, want only the kept file", held)
	}
	if !hasExcludedWalkPart("docs/deep/third_party/vendor.md") {
		t.Fatal("a nested vendored directory must be an excluded part")
	}
	if hasExcludedWalkPart("docs/deep/kept.md") {
		t.Fatal("an ordinary path must not read as excluded")
	}
}

// A cancelled scan is refused mid-sweep rather than reported on the part it
// reached.
func TestACancelledScanIsRefusedRatherThanPartiallyReported(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "notes.md")
	if err := os.WriteFile(path, []byte("5 \u00b0C\n"), 0o644); err != nil {
		t.Fatalf("plant: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var out, errs bytes.Buffer
	if code := Run(ctx, root, []string{"--check", path}, &out, &errs); code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out.String(), errs.String())
	}
	if !strings.Contains(errs.String(), "scan cancelled") {
		t.Fatalf("stderr = %q, want the cancellation named", errs.String())
	}
	if strings.Contains(out.String(), "[NEEDS-FIX]") {
		t.Fatalf("stdout = %q, want no partial report", out.String())
	}
}
