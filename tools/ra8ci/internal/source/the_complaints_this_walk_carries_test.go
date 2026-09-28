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

// theCommit is the object ID the stub below reports for every checkout.
const theCommit = "1f2e3d4c5b6a798807162534435261708f9e0d1c"

// stubbedGit puts a scripted git first on PATH. The snapshot resolves the
// executable off PATH and runs it with a fixed argument vector, so a script
// that dispatches on the subcommand can answer each step of the walk and fail
// exactly one of them. Real git cannot be made to fail these ways on demand.
func stubbedGit(t *testing.T, lsTree string) {
	t.Helper()
	dir := t.TempDir()
	script := "#!/bin/sh\nfor arg in \"$@\"; do\n  case \"$arg\" in\n" +
		"  rev-parse) echo " + theCommit + "; exit 0 ;;\n" +
		"  status) exit 0 ;;\n" +
		"  archive) echo tar-bytes; exit 0 ;;\n" +
		"  ls-tree) " + lsTree + " ;;\n" +
		"  esac\ndone\nexit 0\n"
	path := filepath.Join(dir, "git")
	if err := os.WriteFile(path, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(path, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir)
}

// A checkout cannot be identified without git, and the refusal says which
// piece is missing rather than reporting a checkout that is merely unreadable.
func TestAMissingGitIsNamedAsTheMissingPiece(t *testing.T) {
	t.Setenv("PATH", t.TempDir())

	_, err := Snapshot(context.Background(), t.TempDir())
	if !errors.Is(err, ErrGit) || !strings.Contains(err.Error(), "git is missing") {
		t.Fatalf("a snapshot without git answered %v", err)
	}
}

// Every step of the walk hands its own complaint back, and each names the
// step at fault. A snapshot that collapsed them all into one message would
// leave an operator guessing which git call actually went wrong.
func TestEachStepOfTheWalkCarriesItsOwnComplaint(t *testing.T) {
	for _, one := range []struct {
		name    string
		lsTree  string
		wrapped error
		says    string
	}{
		{
			name:    "a tree record with no tab",
			lsTree:  `printf '160000 commit ` + theCommit + ` libs/dep\000'; exit 0`,
			wrapped: ErrGit,
			says:    "malformed ls-tree record",
		},
		{
			name:    "a tree record whose header is short",
			lsTree:  `printf '160000 commit\tlibs/dep\000'; exit 0`,
			wrapped: ErrGit,
			says:    "malformed ls-tree header",
		},
		{
			name:    "a gitlink pinned to something that is not a commit",
			lsTree:  `printf '160000 commit notacommit\tlibs/dep\000'; exit 0`,
			wrapped: ErrGit,
			says:    "invalid gitlink",
		},
		{
			name:    "a gitlink whose path climbs out of the checkout",
			lsTree:  `printf '160000 commit ` + theCommit + `\t../escape\000'; exit 0`,
			wrapped: ErrUnsafePath,
			says:    "../escape",
		},
		{
			name:    "the tree listing itself failing",
			lsTree:  `echo "ls-tree exploded" >&2; exit 3`,
			wrapped: ErrGit,
			says:    "git ls-tree",
		},
		{
			name:    "the tree listing warning on a clean exit",
			lsTree:  `echo "detached something" >&2; exit 0`,
			wrapped: ErrGit,
			says:    "warning",
		},
	} {
		t.Run(one.name, func(t *testing.T) {
			stubbedGit(t, one.lsTree)

			_, err := Snapshot(context.Background(), t.TempDir())
			if !errors.Is(err, one.wrapped) {
				t.Fatalf("answered %v, want %v", err, one.wrapped)
			}
			if !strings.Contains(err.Error(), one.says) {
				t.Fatalf("the refusal did not say %q: %v", one.says, err)
			}
		})
	}
}

// A commit the checkout reports that is not an object ID is refused rather
// than carried into the manifest, where it would become part of an identity
// nothing can verify.
func TestACommitThatIsNotAnObjectIDIsRefused(t *testing.T) {
	dir := t.TempDir()
	script := "#!/bin/sh\nfor arg in \"$@\"; do\n  case \"$arg\" in\n" +
		"  rev-parse) echo HEAD-is-fine-thanks; exit 0 ;;\n  esac\ndone\nexit 0\n"
	if err := os.WriteFile(filepath.Join(dir, "git"), []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", dir)

	_, err := Snapshot(context.Background(), t.TempDir())
	if !errors.Is(err, ErrGit) || !strings.Contains(err.Error(), "invalid commit") {
		t.Fatalf("a checkout with no real commit answered %v", err)
	}
}

// A pinned submodule has to be on disk as an initialized directory, and each
// of the three ways it can fail to be one is refused in its own words: absent,
// present but not a directory, and a directory that was never initialized.
func TestAPinnedSubmoduleMustBeAnInitializedDirectory(t *testing.T) {
	for _, one := range []struct {
		name  string
		plant func(t *testing.T, root string)
		says  string
	}{
		{
			name:  "absent from the checkout",
			plant: func(t *testing.T, root string) {},
			says:  "uninitialized or escaping",
		},
		{
			name: "present as a file",
			plant: func(t *testing.T, root string) {
				if err := os.MkdirAll(filepath.Join(root, "libs"), 0o755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(root, "libs", "dep"), []byte("not a checkout\n"), 0o600); err != nil {
					t.Fatal(err)
				}
			},
			says: "is not a directory",
		},
		{
			name: "a directory that was never initialized",
			plant: func(t *testing.T, root string) {
				if err := os.MkdirAll(filepath.Join(root, "libs", "dep"), 0o755); err != nil {
					t.Fatal(err)
				}
			},
			says: "is not initialized",
		},
	} {
		t.Run(one.name, func(t *testing.T) {
			stubbedGit(t, `printf '160000 commit `+theCommit+`\tlibs/dep\000'; exit 0`)
			root := t.TempDir()
			one.plant(t, root)

			_, err := Snapshot(context.Background(), root)
			if !errors.Is(err, ErrUnsafePath) {
				t.Fatalf("answered %v, want an unsafe-path refusal", err)
			}
			if !strings.Contains(err.Error(), one.says) || !strings.Contains(err.Error(), "libs/dep") {
				t.Fatalf("the refusal did not say %q of libs/dep: %v", one.says, err)
			}
		})
	}
}
