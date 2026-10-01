// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package source

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// gitAnsweringOnlyAtTheRoot puts a scripted git first on PATH that reports
// gitlinks for the root of the checkout and nothing for anything below it.
// The real walk recurses into every submodule it is told about, so a stub
// that answered the same tree everywhere would descend until it hit the depth
// bound and the snapshot would fail there instead of where the test is
// looking.
func gitAnsweringOnlyAtTheRoot(t *testing.T, canonicalRoot, rootTree string) {
	t.Helper()
	dir := t.TempDir()
	script := "#!/bin/sh\n" +
		"asked=\"$2\"\n" +
		"for arg in \"$@\"; do\n" +
		"  case \"$arg\" in\n" +
		"  rev-parse) echo " + theCommit + "; exit 0 ;;\n" +
		"  status) exit 0 ;;\n" +
		"  archive) echo tar-bytes; exit 0 ;;\n" +
		"  ls-tree)\n" +
		"    if [ \"$asked\" = \"" + canonicalRoot + "\" ]; then " + rootTree + "; fi\n" +
		"    exit 0 ;;\n" +
		"  esac\n" +
		"done\n" +
		"exit 0\n"
	path := filepath.Join(dir, "git")
	if err := os.WriteFile(path, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir)
}

// Two submodules whose paths differ only in case are two directories here and
// one directory on a case-insensitive filesystem. The snapshot is the identity
// a runner verifies its checkout against, so a pair that collapses into one
// entry somewhere else has to be refused where it is still visible as two:
// accepted here, the same tree would check out with one submodule silently
// standing in for the other and still match the digest.
func TestTwoSubmodulesCannotShareOnePathIgnoringCase(t *testing.T) {
	root := t.TempDir()
	canonical, err := filepath.EvalSymlinks(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"Sub", "sub"} {
		if err := os.Mkdir(filepath.Join(canonical, name), 0o755); err != nil {
			t.Skipf("this filesystem cannot hold both spellings at once: %v", err)
		}
		initialized(t, filepath.Join(canonical, name))
	}
	gitAnsweringOnlyAtTheRoot(t, canonical,
		`printf '160000 commit `+theCommit+`\tSub\000160000 commit `+theCommit+`\tsub\000'`)

	_, err = Snapshot(context.Background(), root)
	if !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("err = %v, want ErrUnsafePath", err)
	}
	if !strings.Contains(err.Error(), "colliding submodule paths") {
		t.Fatalf("the refusal does not name the collision: %v", err)
	}
}

// The same walk over two submodules that do not collide is accepted, so the
// refusal above cannot be read as the snapshot refusing sibling submodules in
// general.
func TestTwoSubmodulesWithDistinctPathsAreBothTaken(t *testing.T) {
	root := t.TempDir()
	canonical, err := filepath.EvalSymlinks(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"first", "second"} {
		if err := os.Mkdir(filepath.Join(canonical, name), 0o755); err != nil {
			t.Fatal(err)
		}
		initialized(t, filepath.Join(canonical, name))
	}
	gitAnsweringOnlyAtTheRoot(t, canonical,
		`printf '160000 commit `+theCommit+`\tfirst\000160000 commit `+theCommit+`\tsecond\000'`)

	result, err := Snapshot(context.Background(), root)
	if err != nil {
		t.Fatalf("a checkout with two distinct submodules was refused: %v", err)
	}
	var paths []string
	for _, entry := range result.Manifest.Entries {
		paths = append(paths, entry.Path)
	}
	if strings.Join(paths, ",") != ",first,second" {
		t.Fatalf("entries = %q, want the root and both submodules", paths)
	}
}

// initialized gives a submodule directory the .git entry the walk requires
// before it will descend into it. A real checkout has a gitfile there; the
// walk only asks that something is present, and the scripted git answers for
// the directory either way.
func initialized(t *testing.T, directory string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(directory, ".git"), []byte("gitdir: ../.git/modules/x\n"), 0o644); err != nil {
		t.Fatal(err)
	}
}
