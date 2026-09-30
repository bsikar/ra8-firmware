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

// hostileConfigHome points XDG_CONFIG_HOME at a home the lease directory
// cannot be kept in, and hands back nothing: what each case asserts is the
// refusal, not a path. spoil is given the home and arranges the obstacle.
func hostileConfigHome(t *testing.T, spoil func(home string)) {
	t.Helper()
	home := t.TempDir()
	spoil(home)
	t.Setenv("XDG_CONFIG_HOME", home)
}

// A cancellation names three identifiers and nothing else. Every other shape
// is refused here, before any credential is read, because an operator who
// mistyped a request id must be told about the identifiers rather than about
// a server they were never going to reach.
func TestBoardCancelRefusesATicketItCannotRead(t *testing.T) {
	noServerNamed(t)
	board, request, lease := "ek-ra8d2", soundRunID(t), soundRunID(t)
	const usage = "usage: ra8ci board cancel <board-id> <request-id> <lease-id>"

	for name, args := range map[string][]string{
		"nothing at all":                      {},
		"a board alone":                       {board},
		"no lease":                            {board, request},
		"one identifier too many":             {board, request, lease, lease},
		"a board id that is not one":          {"ek ra8d2", request, lease},
		"a board id with a path in it":        {"../ek-ra8d2", request, lease},
		"a request id that is not one":        {board, "request-1", lease},
		"a lease id that is not one":          {board, request, "lease-1"},
		"the identifiers swapped for empties": {board, "", ""},
	} {
		t.Run(name, func(t *testing.T) {
			err := boardCancelCommand(context.Background(), args)
			if err == nil {
				t.Fatal("the cancellation was accepted")
			}
			if err.Error() != usage {
				t.Fatalf("refusal = %q, want the usage line", err)
			}
		})
	}
}

// Take carries three flags and no others. An unknown flag, or one handed no
// value, is refused with the usage line carrying the parse failure, so the
// operator sees both what they typed wrong and what the command wanted.
func TestBoardTakeRefusesAFlagItDoesNotOffer(t *testing.T) {
	noServerNamed(t)
	for name, args := range map[string][]string{
		"a flag that is not offered":                  {"ek-ra8d2", "--reason", "bring-up"},
		"a misspelling of a real flag":                {"ek-ra8d2", "--durations", "1h"},
		"a flag handed no value":                      {"ek-ra8d2", "--why"},
		"a single-dash long flag that takes no value": {"ek-ra8d2", "-duration"},
	} {
		t.Run(name, func(t *testing.T) {
			err := boardTakeCommand(context.Background(), args)
			if err == nil {
				t.Fatal("the invocation was accepted")
			}
			if !strings.HasPrefix(err.Error(), "usage: ra8ci board take <board-id> --why <reason> --duration <duration>:") {
				t.Fatalf("refusal = %q, want the usage line carrying the parse failure", err)
			}
		})
	}
}

// Extend and heartbeat both reach for the local lease directory before they
// reach for credentials, so a configuration home that cannot hold one
// privately stops them there. The lease token is the board's only proof of
// who holds it; writing one into a directory other people can read would
// hand the bench away, which is why this refuses rather than falls back.
func TestBoardCommandsRefuseAConfigurationHomeTheyCannotKeepALeaseIn(t *testing.T) {
	commands := map[string]func(context.Context) error{
		"extend": func(ctx context.Context) error {
			return boardExtendCommand(ctx, []string{"ek-ra8d2", "--why", "bring-up", "--duration", "30m"})
		},
		"heartbeat": func(ctx context.Context) error {
			return boardHeartbeatCommand(ctx, []string{"ek-ra8d2"})
		},
	}
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

	for commandName, command := range commands {
		for obstacleName, obstacle := range obstacles {
			t.Run(commandName+" against "+obstacleName, func(t *testing.T) {
				noServerNamed(t)
				hostileConfigHome(t, obstacle.spoil)
				err := command(context.Background())
				if err == nil {
					t.Fatal("the command carried on without a private lease directory")
				}
				if err.Error() != obstacle.want {
					t.Fatalf("refusal = %q, want %q", err, obstacle.want)
				}
			})
		}
	}
}

// The same two commands judge their arguments ahead of the directory, so a
// board name that is not one is reported as such even when the configuration
// home is also unusable. Reversing that order would tell an operator to fix
// their home when the thing they mistyped was the board.
func TestBoardCommandsJudgeTheBoardNameBeforeTheConfigurationHome(t *testing.T) {
	noServerNamed(t)
	hostileConfigHome(t, func(home string) {
		if err := os.WriteFile(filepath.Join(home, "ra8ci"), []byte("not a directory\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	})
	if err := boardHeartbeatCommand(context.Background(), []string{"ek ra8d2"}); err == nil ||
		err.Error() != "usage: ra8ci board heartbeat <board-id>" {
		t.Fatalf("refusal = %v, want the board name refused ahead of the home", err)
	}
	if err := boardExtendCommand(context.Background(),
		[]string{"ek-ra8d2", "--why", "bring-up", "--duration", "30ms"}); err == nil ||
		!strings.Contains(err.Error(), "whole number of seconds") {
		t.Fatalf("refusal = %v, want the duration refused ahead of the home", err)
	}
}
