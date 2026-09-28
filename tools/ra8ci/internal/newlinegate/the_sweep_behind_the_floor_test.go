// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The sweep behind the two floors, and the report it writes when files do
// come back missing a newline. Both floors exist to stop a collapsed scan
// from reporting a clean tree, so both have to be pinned as refusals.

func plantRepo(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for rel, body := range files {
		path := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
			t.Fatalf("plant %s: %v", rel, err)
		}
		if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
			t.Fatalf("plant %s: %v", rel, err)
		}
	}
	command := exec.Command("git", "init", "-q", root)
	if out, err := command.CombinedOutput(); err != nil {
		t.Skipf("git init unavailable on this box: %v (%s)", err, out)
	}
	return root
}

func digits(value int) string {
	if value == 0 {
		return "0000"
	}
	out := []byte("0000")
	for index := 3; index >= 0 && value > 0; index-- {
		out[index] = byte('0' + value%10)
		value /= 10
	}
	return string(out)
}

// A tree with plenty of tracked paths but almost nothing in scope is a
// collapsed sweep, and the gate has to say which floor it fell through
// rather than call the tree clean.
func TestASweepWithTooFewFilesInScopeIsRefusedNotCalledClean(t *testing.T) {
	files := make(map[string]string, trackedFloor+200)
	for index := 0; index < trackedFloor+200; index++ {
		files["blobs/opaque"+digits(index)+".bin"] = "no newline here"
	}
	code, out, errs := scan(t, plantRepo(t, files), "--all")
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, "floor is "+digits(fileFloor)[len(digits(fileFloor))-4:]) && !strings.Contains(errs, "floor is 2200") {
		t.Fatalf("stderr = %q, want the file floor named", errs)
	}
	if strings.Contains(out, "all end in a newline") {
		t.Fatalf("stdout = %q, must never call a collapsed sweep clean", out)
	}
}

// Too few TRACKED paths is the earlier of the two floors and is reported as
// its own refusal, because it means the repository itself was not read,
// not that the scope rules dropped everything.
func TestARepositoryWithTooFewTrackedPathsIsRefusedOnItsOwnTerms(t *testing.T) {
	code, out, errs := scan(t, plantRepo(t, map[string]string{"apps/main.c": "int main(void) { return 0; }\n"}), "--all")
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, "tracked path(s), floor is 1000") {
		t.Fatalf("stderr = %q, want the tracked floor named", errs)
	}
	if strings.Contains(errs, "file(s) in scope") {
		t.Fatalf("stderr = %q, a tracked-floor refusal is not a scope-floor refusal", errs)
	}
}

// A directory that is not a repository at all fails the derivation rather
// than sweeping nothing and passing.
func TestASweepOutsideARepositoryIsRefused(t *testing.T) {
	code, out, errs := scan(t, t.TempDir(), "--all")
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, "git ls-files failed") {
		t.Fatalf("stderr = %q, want the derivation named", errs)
	}
}

// A full sweep that finds nothing reports how many files it actually read,
// which is the number that lets a reviewer tell a real sweep from a
// collapsed one.
func TestAFullSweepReportsHowManyFilesItRead(t *testing.T) {
	files := make(map[string]string, fileFloor)
	for index := 0; index < fileFloor; index++ {
		files["apps/unit"+digits(index)+".c"] = "/* unit */\n"
	}
	root := plantRepo(t, files)
	code, out, errs := scan(t, root, "--all")
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "2200 file(s) scanned, all end in a newline.") {
		t.Fatalf("stdout = %q, want the scanned count", out)
	}
	if errs != "" {
		t.Fatalf("stderr = %q, want nothing", errs)
	}

	// The same sweep with one offender fails and names only that file.
	if err := os.WriteFile(filepath.Join(root, "apps", "unit0007.c"), []byte("/* no newline */"), 0o600); err != nil {
		t.Fatalf("spoil a unit: %v", err)
	}
	code, out, errs = scan(t, root, "--all")
	if code != 1 {
		t.Fatalf("spoiled: code = %d, want 1 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(errs, "apps/unit0007.c") {
		t.Fatalf("stderr = %q, want the offender named", errs)
	}
	if !strings.Contains(errs, "1 file(s) missing a trailing newline") {
		t.Fatalf("stderr = %q, want exactly one counted", errs)
	}
}

// --all named beside explicit paths is an ambiguous invocation, refused with
// the usage line rather than silently taking one meaning over the other.
func TestAllNamedBesideExplicitPathsIsRefused(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "apps/main.c", "int main(void) { return 0; }\n")
	for _, args := range [][]string{
		{"--all", "apps/main.c"},
		{"apps/main.c", "--all"},
		{"--all", "--all"},
	} {
		code, out, errs := scan(t, root, args...)
		if code != 2 {
			t.Fatalf("args %q: code = %d, want 2", args, code)
		}
		if !strings.Contains(errs, "usage: ra8ci final-newline") {
			t.Fatalf("args %q: stderr = %q, want the usage line", args, errs)
		}
		if out != "" {
			t.Fatalf("args %q: stdout = %q, want nothing", args, out)
		}
	}
}

// The offenders are reported sorted and carry the instruction that says what
// to do about them, on stderr, while stdout stays empty: a failing gate has
// no clean verdict to give.
func TestTheOffendersAreReportedSortedWithTheirRemedy(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "zeta.c", "last")
	sourceFile(t, root, "alpha.c", "first")
	sourceFile(t, root, "middle.c", "fine\n")
	code, out, errs := scan(t, root, "zeta.c", "alpha.c", "middle.c")
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(errs, "2 file(s) missing a trailing newline") {
		t.Fatalf("stderr = %q, want both counted", errs)
	}
	if strings.Index(errs, "alpha.c") > strings.Index(errs, "zeta.c") {
		t.Fatalf("stderr = %q, want the offenders sorted", errs)
	}
	if strings.Contains(errs, "middle.c") {
		t.Fatalf("stderr = %q, a file that ends in a newline is not an offender", errs)
	}
	if !strings.Contains(errs, "Add a single newline at end of file.") {
		t.Fatalf("stderr = %q, want the remedy", errs)
	}
	if out != "" {
		t.Fatalf("stdout = %q, want no verdict", out)
	}
}

// A file outside the root keeps its absolute path in the report. Relative
// wording would name a path that does not exist from where the reader is
// standing.
func TestAnOffenderOutsideTheRootKeepsItsAbsolutePath(t *testing.T) {
	root := t.TempDir()
	elsewhere := sourceFile(t, t.TempDir(), "stray.c", "no newline")
	code, _, errs := scan(t, root, elsewhere)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	if !strings.Contains(errs, elsewhere) {
		t.Fatalf("stderr = %q, want the absolute path %q", errs, elsewhere)
	}
}

// A cancelled sweep is refused before it can finish, and it says so rather
// than reporting on the part it happened to get through.
func TestACancelledSweepIsRefusedRatherThanPartiallyReported(t *testing.T) {
	root := t.TempDir()
	path := sourceFile(t, root, "apps/main.c", "int main(void) { return 0; }\n")
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var out, errs strings.Builder
	if code := Run(ctx, root, []string{path}, &out, &errs); code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out.String(), errs.String())
	}
	if !strings.Contains(errs.String(), "scan cancelled") {
		t.Fatalf("stderr = %q, want the cancellation named", errs.String())
	}
	if out.String() != "" {
		t.Fatalf("stdout = %q, want no verdict", out.String())
	}
}
