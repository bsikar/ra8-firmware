// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package source

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// scriptedGit injects one response for a Git subcommand without relying on a
// platform-specific executable script.
func scriptedGit(t *testing.T, subcommand string, response fakeGitResponse) {
	t.Helper()
	installFakeGit(t, fakeGitFixture{Responses: map[string]fakeGitResponse{subcommand: response}})
}

// A digest is only worth as much as the archive it was taken over. git can
// exit 0 having written a warning, and a tar that stopped early still hashes
// to something, so a snapshot that ignored the warning would publish a
// confident digest of a truncated tree. Both halves are refused: the archive
// that fails outright, and the one that succeeds while complaining.
func TestADigestIsNotTakenFromAGitThatComplained(t *testing.T) {
	for _, one := range []struct {
		name     string
		response fakeGitResponse
		says     string
	}{
		{
			name:     "archive fails",
			response: fakeGitResponse{Stderr: []byte("fatal: pack is corrupt\n"), Exit: 128},
			says:     "archive",
		},
		{
			// Exit 0, real bytes on stdout, a warning on stderr. This is the
			// one that would otherwise pass silently.
			name:     "archive warns but exits zero",
			response: fakeGitResponse{Stdout: []byte("tar-bytes\n"), Stderr: []byte("warning: ignoring broken symlink\n")},
			says:     "archive warning",
		},
	} {
		t.Run(one.name, func(t *testing.T) {
			scriptedGit(t, "archive", one.response)

			_, err := Snapshot(context.Background(), t.TempDir())
			if !errors.Is(err, ErrGit) {
				t.Fatalf("answered %v, want an ErrGit refusal", err)
			}
			if !strings.Contains(err.Error(), one.says) {
				t.Fatalf("the refusal does not name the step at fault: %v", err)
			}
		})
	}
}

// The working-tree check is what decides a checkout is clean enough to
// identify. A status that cannot run at all is not a clean tree, and the
// refusal has to say so rather than fall through to a digest.
func TestAStatusThatCannotRunIsNotACleanTree(t *testing.T) {
	for _, one := range []struct {
		name     string
		response fakeGitResponse
		says     string
	}{
		{
			name:     "status fails",
			response: fakeGitResponse{Stderr: []byte("fatal: index file is unreadable\n"), Exit: 128},
			says:     "git status",
		},
		{
			name:     "status warns but exits zero",
			response: fakeGitResponse{Stderr: []byte("warning: fsmonitor is unavailable\n")},
			says:     "git status warning",
		},
	} {
		t.Run(one.name, func(t *testing.T) {
			scriptedGit(t, "status", one.response)

			_, err := Snapshot(context.Background(), t.TempDir())
			if !errors.Is(err, ErrGit) {
				t.Fatalf("answered %v, want an ErrGit refusal", err)
			}
			if !strings.Contains(err.Error(), one.says) {
				t.Fatalf("the refusal does not name the step at fault: %v", err)
			}
			if errors.Is(err, ErrDirty) {
				t.Fatalf("a status that never ran was reported as a dirty tree: %v", err)
			}
		})
	}
}
