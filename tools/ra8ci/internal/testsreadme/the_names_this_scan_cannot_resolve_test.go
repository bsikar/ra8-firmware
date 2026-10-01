// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A name the directory listing hands back but the filesystem will not resolve
// is passed over, not counted as an undocumented subdirectory. A dangling
// symlink is the ordinary way that happens: it is listed, and stat says the
// thing it points at is not there.
func TestANameThatDoesNotResolveIsPassedOver(t *testing.T) {
	names := fiveNames()
	root := plantRoot(t, names, names...)
	if err := os.Symlink(filepath.Join(root, "tests", "nothing_here"), filepath.Join(root, "tests", "zeta")); err != nil {
		t.Skipf("symlinks are unavailable: %v", err)
	}

	got := check(t, root)
	if got.code != 0 {
		t.Fatalf("a tree whose only extra name is a dangling symlink answered %+v", got)
	}
	if strings.Contains(got.stdout, "zeta") || strings.Contains(got.stderr, "zeta") {
		t.Fatalf("the unresolved name was judged: %q %q", got.stdout, got.stderr)
	}
}

// A name that fails to resolve for any other reason is a refusal that names
// the path, rather than a name quietly dropped from the count. A symlink
// pointing at itself is the cheapest way to get an error that is not
// "does not exist".
func TestANameThatCannotBeStattedIsARefusal(t *testing.T) {
	names := fiveNames()
	root := plantRoot(t, names, names...)
	loop := filepath.Join(root, "tests", "zeta")
	if err := os.Symlink("zeta", loop); err != nil {
		t.Skipf("symlinks are unavailable: %v", err)
	}
	if _, err := os.Stat(loop); err == nil {
		t.Skip("this filesystem resolves a self-referential symlink")
	}

	got := check(t, root)
	if got.code != 2 {
		t.Fatalf("a tree with an unstattable name answered %+v", got)
	}
	if !strings.Contains(got.stderr, "zeta") {
		t.Fatalf("the refusal did not name the path at fault: %q", got.stderr)
	}
	if got.stdout != "" {
		t.Fatalf("stdout carried %q", got.stdout)
	}
}

// A table row is read for its first cell, and a row that has no second cell is
// not a table row at all. It is prose that happens to start with a pipe, and
// reading a directory name out of it would document a subdirectory the README
// never described.
func TestARowWithNoSecondCellIsNotDocumentation(t *testing.T) {
	for _, line := range []string{
		"| `alpha/`",
		"| `alpha/` ",
		"|`alpha/`|",
	} {
		readme := "# tests\n\n| directory | what it holds |\n| --- | --- |\n" + line + "\n"
		if got := documented(readme); len(got) != 0 {
			t.Fatalf("the line %q was read as documentation: %v", line, got)
		}
	}
	whole := "# tests\n\n| directory | what it holds |\n| --- | --- |\n| `alpha/` | fixture description |\n"
	if got := documented(whole); len(got) != 1 || got[0] != "alpha" {
		t.Fatalf("a complete row was not read as documentation: %v", got)
	}
}
