// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

// The gate reads a target twice: once to judge it and, when it is asked to
// rewrite, once more to write it back. Between those two moments the file
// can turn out not to be a file at all, or to hold bytes that are not text.
// What matters is that each of those endings is a refusal naming the
// target, never a silent zero and never a half-written file.
//
// The box these tests run on is root, so a permission wedge proves nothing:
// chmod 0000 is still readable. The honest wedge is a TYPE error. A
// directory where a file belongs answers EISDIR from a read, and neither
// that nor a named pipe is os.ErrNotExist, so both reach the "cannot read"
// arms rather than the "missing" ones.

func readableFile(t *testing.T, dir, name, body string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// A directory handed in where a file was expected is a read failure that
// names the target, not an empty scan.
func TestADirectoryInPlaceOfAFileIsAReadFailure(t *testing.T) {
	dir := t.TempDir()
	nested := filepath.Join(dir, "docs")
	if err := os.MkdirAll(nested, 0o755); err != nil {
		t.Fatal(err)
	}

	count, err := process(nested, false)
	if err == nil {
		t.Fatalf("a directory scanned as a file, count = %d", count)
	}
	if !strings.Contains(err.Error(), nested) {
		t.Fatalf("err = %v, want it to name the target", err)
	}
	if count != 0 {
		t.Fatalf("count = %d, want 0 alongside the refusal", count)
	}
}

// Bytes that are not text are refused outright rather than transliterated,
// because a trusted ASCII rewrite of a binary would corrupt it.
func TestBytesThatAreNotTextAreRefusedRatherThanRewritten(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "firmware.bin")
	original := []byte{0xff, 0xfe, 0x00, 0x80, 0x41}
	if err := os.WriteFile(path, original, 0o644); err != nil {
		t.Fatal(err)
	}

	for _, rewrite := range []bool{false, true} {
		count, err := process(path, rewrite)
		if err == nil {
			t.Fatalf("rewrite=%v: invalid UTF-8 was scanned, count = %d", rewrite, count)
		}
		if !strings.Contains(err.Error(), "invalid UTF-8") || !strings.Contains(err.Error(), path) {
			t.Fatalf("rewrite=%v: err = %v, want the UTF-8 refusal naming the target", rewrite, err)
		}
	}

	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != string(original) {
		t.Fatal("a refused file was written to anyway")
	}
}

// A rewrite replaces what it found, keeps the mode it was stored under, and
// reports the count of characters it had to replace. A second pass over the
// rewritten file finds nothing left to do.
func TestARewriteKeepsTheModeAndSettlesOnTheSecondPass(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "notes.md")
	if err := os.WriteFile(path, []byte("a \u2014 b \u201cquoted\u201d \u00e9\r\nnext\r\n"), 0o640); err != nil {
		t.Fatal(err)
	}

	count, err := process(path, true)
	if err != nil {
		t.Fatalf("rewrite: %v", err)
	}
	if count != 4 {
		t.Fatalf("count = %d, want the four characters above 0x7f", count)
	}

	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, seen := range []string{"\u2014", "\u201c", "\u201d", "\r"} {
		if strings.Contains(string(after), seen) {
			t.Fatalf("the rewrite left %q behind: %q", seen, after)
		}
	}
	if !strings.Contains(string(after), "?") {
		t.Fatalf("an unmapped character was dropped rather than marked: %q", after)
	}

	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o640 {
		t.Fatalf("mode = %v, want the mode the file was stored under", info.Mode().Perm())
	}

	settled, err := process(path, true)
	if err != nil || settled != 0 {
		t.Fatalf("second pass = %d, %v; want a settled file", settled, err)
	}
}

// Judging without rewriting leaves the file exactly as it was, which is
// what lets CI report the same count a developer sees before they fix it.
func TestJudgingWithoutRewritingTouchesNothing(t *testing.T) {
	dir := t.TempDir()
	body := "caf\u00e9 \u2014 na\u00efve\n"
	path := readableFile(t, dir, "page.md", body)

	count, err := process(path, false)
	if err != nil {
		t.Fatal(err)
	}
	if count != 3 {
		t.Fatalf("count = %d, want the three characters above 0x7f", count)
	}
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != body {
		t.Fatalf("a judged file was changed: %q", after)
	}
}

// The checkout path judges the target again under the root it was handed,
// so a target that stopped being a regular file between the scope and the
// scan is refused rather than read through a link.
func TestACheckoutTargetThatStoppedBeingAFileIsRefused(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "docs", "page.md"), 0o755); err != nil {
		t.Fatal(err)
	}
	readableFile(t, root, "real.md", "plain\n")
	if err := os.Symlink("real.md", filepath.Join(root, "linked.md")); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Mkfifo(filepath.Join(root, "pipe.md"), 0o644); err != nil {
		t.Fatal(err)
	}

	for _, target := range []string{"docs/page.md", "linked.md", "pipe.md", "absent.md"} {
		count, err := processCheckout(root, target, false)
		if err == nil {
			t.Fatalf("%s: was scanned, count = %d", target, count)
		}
		if !strings.Contains(err.Error(), "no longer a regular file") {
			t.Fatalf("%s: err = %v, want the regular-file refusal", target, err)
		}
	}
}

// Inside the checkout the same two endings hold: bytes that are not text
// are refused, and a rewrite keeps the mode and settles.
func TestACheckoutRewriteKeepsTheModeAndRefusesBinary(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "firmware.bin"), []byte{0xff, 0xfe}, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(root, "docs"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "docs", "page.md"), []byte("a \u2014 b\r\n"), 0o600); err != nil {
		t.Fatal(err)
	}

	if _, err := processCheckout(root, "firmware.bin", true); err == nil {
		t.Fatal("invalid UTF-8 was scanned inside the checkout")
	} else if !strings.Contains(err.Error(), "invalid UTF-8") {
		t.Fatalf("err = %v, want the UTF-8 refusal", err)
	}
	if raw, err := os.ReadFile(filepath.Join(root, "firmware.bin")); err != nil || len(raw) != 2 {
		t.Fatalf("a refused file was written to anyway: %q %v", raw, err)
	}

	count, err := processCheckout(root, "docs/page.md", true)
	if err != nil {
		t.Fatalf("rewrite: %v", err)
	}
	if count != 1 {
		t.Fatalf("count = %d, want 1", count)
	}
	after, err := os.ReadFile(filepath.Join(root, "docs", "page.md"))
	if err != nil {
		t.Fatal(err)
	}
	// An em dash is replaced by two hyphens, not one, so the width of the
	// line survives the rewrite.
	if string(after) != "a -- b\n" {
		t.Fatalf("rewritten as %q", after)
	}
	info, err := os.Stat(filepath.Join(root, "docs", "page.md"))
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Fatalf("mode = %v, want 0600", info.Mode().Perm())
	}

	settled, err := processCheckout(root, "docs/page.md", true)
	if err != nil || settled != 0 {
		t.Fatalf("second pass = %d, %v; want a settled file", settled, err)
	}
}

// A checkout target judged but not rewritten is left alone, and the slash
// form the scope hands back is accepted on a platform separator.
func TestACheckoutTargetIsJudgedInTheSlashFormTheScopeGives(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "docs"), 0o755); err != nil {
		t.Fatal(err)
	}
	body := "caf\u00e9\n"
	if err := os.WriteFile(filepath.Join(root, "docs", "page.md"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}

	targets, err := checkoutTargets(root, "docs/page.md")
	if err != nil || len(targets) != 1 {
		t.Fatalf("scope = %v, %v", targets, err)
	}
	count, err := processCheckout(root, targets[0], false)
	if err != nil || count != 1 {
		t.Fatalf("judged = %d, %v; want one replacement and no error", count, err)
	}
	after, err := os.ReadFile(filepath.Join(root, "docs", "page.md"))
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != body {
		t.Fatalf("a judged checkout file was changed: %q", after)
	}
}

// A target that is neither a regular file nor a directory is named as such,
// rather than walked into an empty scan.
func TestAPipeIsNeitherAFileNorADirectory(t *testing.T) {
	dir := t.TempDir()
	pipe := filepath.Join(dir, "pipe")
	if err := syscall.Mkfifo(pipe, 0o644); err != nil {
		t.Fatal(err)
	}

	targets, err := walkTargets(pipe)
	if err == nil {
		t.Fatalf("a pipe was accepted as a target: %v", targets)
	}
	if !strings.Contains(err.Error(), "not a regular file or directory") {
		t.Fatalf("err = %v, want the shape refusal", err)
	}
	if targets != nil {
		t.Fatalf("targets = %v, want none alongside the refusal", targets)
	}
}
