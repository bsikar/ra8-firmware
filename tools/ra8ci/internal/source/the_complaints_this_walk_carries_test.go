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
		name     string
		response fakeGitResponse
		wrapped  error
		says     string
	}{
		{
			name:     "a tree record with no tab",
			response: fakeGitResponse{Stdout: []byte("160000 commit " + theCommit + " libs/dep\x00")},
			wrapped:  ErrGit,
			says:     "malformed ls-tree record",
		},
		{
			name:     "a tree record whose header is short",
			response: fakeGitResponse{Stdout: []byte("160000 commit\tlibs/dep\x00")},
			wrapped:  ErrGit,
			says:     "malformed ls-tree header",
		},
		{
			name:     "a gitlink pinned to something that is not a commit",
			response: fakeGitResponse{Stdout: []byte("160000 commit notacommit\tlibs/dep\x00")},
			wrapped:  ErrGit,
			says:     "invalid gitlink",
		},
		{
			name:     "a gitlink whose path climbs out of the checkout",
			response: fakeGitResponse{Stdout: []byte("160000 commit " + theCommit + "\t../escape\x00")},
			wrapped:  ErrUnsafePath,
			says:     "../escape",
		},
		{
			name:     "the tree listing itself failing",
			response: fakeGitResponse{Stderr: []byte("ls-tree exploded\n"), Exit: 3},
			wrapped:  ErrGit,
			says:     "git ls-tree",
		},
		{
			name:     "the tree listing warning on a clean exit",
			response: fakeGitResponse{Stderr: []byte("detached something\n")},
			wrapped:  ErrGit,
			says:     "warning",
		},
	} {
		t.Run(one.name, func(t *testing.T) {
			stubbedGitResponse(t, one.response)

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
	installFakeGit(t, fakeGitFixture{Responses: map[string]fakeGitResponse{
		"rev-parse": {Stdout: []byte("HEAD-is-fine-thanks\n")},
	}})

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
			stubbedGit(t, []byte("160000 commit "+theCommit+"\tlibs/dep\x00"))
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
