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

// scriptedGit puts a git on PATH whose answer to one subcommand is supplied by
// the caller. stubbedGit in the sibling file varies only ls-tree; the digest
// steps need a git that can fail, or succeed while complaining, on status and
// archive in turn. Real git cannot be made to do either on demand.
func scriptedGit(t *testing.T, subcommand, body string) {
	t.Helper()
	dir := t.TempDir()
	script := "#!/bin/sh\nfor arg in \"$@\"; do\n  case \"$arg\" in\n" +
		"  rev-parse) echo " + theCommit + "; exit 0 ;;\n" +
		"  " + subcommand + ") " + body + " ;;\n" +
		"  status) exit 0 ;;\n" +
		"  archive) echo tar-bytes; exit 0 ;;\n" +
		"  ls-tree) exit 0 ;;\n" +
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

// A digest is only worth as much as the archive it was taken over. git can
// exit 0 having written a warning, and a tar that stopped early still hashes
// to something, so a snapshot that ignored the warning would publish a
// confident digest of a truncated tree. Both halves are refused: the archive
// that fails outright, and the one that succeeds while complaining.
func TestADigestIsNotTakenFromAGitThatComplained(t *testing.T) {
	for _, one := range []struct {
		name string
		body string
		says string
	}{
		{
			name: "archive fails",
			body: "echo 'fatal: pack is corrupt' >&2; exit 128",
			says: "archive",
		},
		{
			// Exit 0, real bytes on stdout, a warning on stderr. This is the
			// one that would otherwise pass silently.
			name: "archive warns but exits zero",
			body: "echo tar-bytes; echo 'warning: ignoring broken symlink' >&2; exit 0",
			says: "archive warning",
		},
	} {
		t.Run(one.name, func(t *testing.T) {
			scriptedGit(t, "archive", one.body)

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
		name string
		body string
		says string
	}{
		{
			name: "status fails",
			body: "echo 'fatal: index file is unreadable' >&2; exit 128",
			says: "git status",
		},
		{
			name: "status warns but exits zero",
			body: "echo 'warning: fsmonitor is unavailable' >&2; exit 0",
			says: "git status warning",
		},
	} {
		t.Run(one.name, func(t *testing.T) {
			scriptedGit(t, "status", one.body)

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
