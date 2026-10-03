//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// soundOperatorSurface binds an authority and an operator identity that both
// load, so a command reaches the step after its client is built. Nothing here
// runs a plane: the client is constructed and never used.
func soundOperatorSurface(t *testing.T) {
	t.Helper()
	material := mintReportMaterial(t)
	bindReportEnvironment(t, material, "https://ra8ci.example:8443")
}

// Checkpoint builds its client before it looks for the lease directory, which
// is the opposite order from extend and heartbeat. So its refusal of a
// configuration home that cannot hold a lease privately is only reachable
// once the credentials load, and it was uncovered for exactly that reason.
//
// The refusal itself matters for the same reason it does on the other two
// commands: the lease token is the board's only proof of who holds the bench,
// and a home other people can read would hand that away.
func TestBoardCheckpointRefusesAConfigurationHomeItCannotKeepALeaseIn(t *testing.T) {
	obstacles := map[string]struct {
		spoil func(home string)
		want  string
	}{
		"a home that is a file": {
			spoil: func(home string) {
				if err := os.WriteFile(filepath.Join(home, "ra8ci"), []byte("not a directory\n"), 0o600); err != nil {
					t.Fatal(err)
				}
			},
			want: "create private ra8ci configuration directory",
		},
		"a directory others can read": {
			spoil: func(home string) {
				if err := os.Mkdir(filepath.Join(home, "ra8ci"), 0o755); err != nil {
					t.Fatal(err)
				}
			},
			want: "ra8ci configuration directory must be private and not a symlink",
		},
		"a directory that is really a symlink": {
			spoil: func(home string) {
				elsewhere := filepath.Join(home, "elsewhere")
				if err := os.Mkdir(elsewhere, 0o700); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(elsewhere, filepath.Join(home, "ra8ci")); err != nil {
					t.Fatal(err)
				}
			},
			want: "ra8ci configuration directory must be private and not a symlink",
		},
	}

	for name, obstacle := range obstacles {
		t.Run(name, func(t *testing.T) {
			soundOperatorSurface(t)
			hostileConfigHome(t, obstacle.spoil)

			err := boardCheckpointCommand(context.Background(), []string{"ek-ra8d2"})
			if err == nil {
				t.Fatal("the checkpoint carried on without a private lease directory")
			}
			if err.Error() != obstacle.want {
				t.Fatalf("refusal = %q, want %q", err, obstacle.want)
			}
			// The directory refusal is handed back bare. Wrapping it in
			// "board checkpoint:" would read as the server having refused
			// the checkpoint, when nothing was ever sent.
			if strings.HasPrefix(err.Error(), "board checkpoint:") {
				t.Fatalf("refusal = %q, want the home reported rather than a checkpoint that never went out", err)
			}
		})
	}
}

// With credentials that load and a home that can hold a lease, the only thing
// missing is the lease itself. That refusal is the checkpoint's own, so it
// carries the command's prefix, which is the difference from the cases above.
func TestBoardCheckpointWithNoLeaseHeldNamesTheCheckpoint(t *testing.T) {
	soundOperatorSurface(t)
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())

	err := boardCheckpointCommand(context.Background(), []string{"ek-ra8d2"})
	if err == nil {
		t.Fatal("a checkpoint was accepted with no lease held on this machine")
	}
	if !strings.HasPrefix(err.Error(), "board checkpoint: ") {
		t.Fatalf("refusal = %q, want the checkpoint to name itself", err)
	}
}

// The board name is judged ahead of everything: ahead of the credentials and
// ahead of the home. An operator who mistyped the board must be told about
// the board, not sent to fix an identity that was never the problem.
func TestBoardCheckpointJudgesTheBoardNameAheadOfEverything(t *testing.T) {
	noServerNamed(t)
	hostileConfigHome(t, func(home string) {
		if err := os.WriteFile(filepath.Join(home, "ra8ci"), []byte("not a directory\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	})
	const usage = "usage: ra8ci board checkpoint <board-id>"
	for name, args := range map[string][]string{
		"nothing at all":           {},
		"two boards":               {"ek-ra8d2", "ek-ra8m1"},
		"a name with a space":      {"ek ra8d2"},
		"a name with a path in it": {"../ek-ra8d2"},
		"an empty name":            {""},
		"a name with a newline":    {"ek-ra8d2\n"},
	} {
		t.Run(name, func(t *testing.T) {
			err := boardCheckpointCommand(context.Background(), args)
			if err == nil || err.Error() != usage {
				t.Fatalf("refusal = %v, want the usage line ahead of the credentials and the home", err)
			}
		})
	}
}
