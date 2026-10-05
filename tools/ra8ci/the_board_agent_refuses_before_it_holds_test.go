// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

// boardAgentSurface sets the whole board-agent environment to values that
// resolve, with the state file in a private directory. Each test below spoils
// exactly one part of it, so the refusal proves that part and nothing else.
// The credentials are paths that do not exist: every refusal under test is
// taken before any of them is read.
func boardAgentSurface(t *testing.T) string {
	t.Helper()
	if runtime.GOOS != "linux" {
		t.Skip("board-agent service configuration and state checks run only on Linux; other hosts are refused before service setup")
	}
	// The state store refuses any directory the rest of the machine can read
	// or write. Protect the fixture explicitly rather than relying on its
	// inherited ACL or mode.
	home := filepath.Join(t.TempDir(), "private")
	if err := os.Mkdir(home, 0o700); err != nil {
		t.Fatalf("plant private directory: %v", err)
	}
	if err := testprivatefile.OwnerOnly(home); err != nil {
		t.Fatalf("restrict private directory to its owner: %v", err)
	}
	t.Setenv(envServerURL, "https://ra8ci.invalid:8443")
	t.Setenv(envServerCA, filepath.Join(home, "ca.pem"))
	t.Setenv(envBoardAgentCert, filepath.Join(home, "agent.pem"))
	t.Setenv(envBoardAgentKey, filepath.Join(home, "agent.key"))
	t.Setenv(envBoardID, "ek-ra8d2")
	t.Setenv(envBoardStateFile, filepath.Join(home, "highwater.json"))
	t.Setenv("RA8CI_BOARD_AGENT_POLL_INTERVAL", "")
	return home
}

func TestTheBoardAgentNamesOnlyTheVariablesActuallyMissing(t *testing.T) {
	everything := []string{envServerURL, envServerCA, envBoardAgentCert, envBoardAgentKey,
		envBoardID, envBoardStateFile}

	t.Run("all of them, in the order the surface is read", func(t *testing.T) {
		boardAgentSurface(t)
		for _, name := range everything {
			t.Setenv(name, "")
		}
		err := runBoardAgent(context.Background())
		if err == nil {
			t.Fatal("the board agent started with no surface configured")
		}
		want := "ra8ci board-agent: set " + strings.Join(everything[:len(everything)-1], ", ") +
			" and " + everything[len(everything)-1]
		if err.Error() != want {
			t.Fatalf("err=%q; want exactly %q", err, want)
		}
	})

	// One missing variable is named on its own, with no "and", and none of the
	// five the operator already set are mentioned: a refusal that lists the
	// whole surface makes them re-check what was never wrong.
	for _, missing := range everything {
		t.Run("only "+missing, func(t *testing.T) {
			boardAgentSurface(t)
			t.Setenv(missing, "")
			err := runBoardAgent(context.Background())
			if err == nil || err.Error() != "ra8ci board-agent: set "+missing {
				t.Fatalf("err=%v; want exactly the one missing variable %s", err, missing)
			}
			for _, other := range everything {
				if other != missing && strings.Contains(err.Error(), other) {
					t.Fatalf("err=%q; want no mention of %s, which is set", err, other)
				}
			}
		})
	}

	// A value that is only whitespace would otherwise reach os.ReadFile as a
	// path and fail as a missing file. It reads as absent instead.
	for _, blank := range []string{" ", "\t", "\n", "   \t "} {
		t.Run("a value that is only whitespace", func(t *testing.T) {
			boardAgentSurface(t)
			t.Setenv(envBoardID, blank)
			err := runBoardAgent(context.Background())
			if err == nil || err.Error() != "ra8ci board-agent: set "+envBoardID {
				t.Fatalf("err=%v for %q; want it read as absent", err, blank)
			}
		})
	}
}

func TestTheBoardAgentRefusesAPollIntervalOutsideItsBounds(t *testing.T) {
	refused := "RA8CI_BOARD_AGENT_POLL_INTERVAL must be between 250ms and 30s"
	for name, value := range map[string]string{
		"a duration that is not one": "often",
		"no time at all":             "0s",
		"time going backwards":       "-1s",
		"under the floor":            "249ms",
		"far under the floor":        "1ms",
		"over the ceiling":           "30s1ms",
		"far over the ceiling":       "10m",
	} {
		t.Run(name, func(t *testing.T) {
			boardAgentSurface(t)
			t.Setenv("RA8CI_BOARD_AGENT_POLL_INTERVAL", value)
			err := runBoardAgent(context.Background())
			if err == nil || err.Error() != refused {
				t.Fatalf("err=%v; want exactly %q", err, refused)
			}
			// The interval is judged before the state file is opened and
			// before any credential is read, so a bad one leaves no state
			// behind and never touches the key pair.
			if strings.Contains(err.Error(), "board-agent state") ||
				strings.Contains(err.Error(), "board-agent client") {
				t.Fatalf("err=%v; want the refusal taken before the state file and the client", err)
			}
		})
	}

	// Both bounds are inclusive, so each gets past the interval check and is
	// stopped by the credentials that do not exist.
	for name, value := range map[string]string{"the floor": "250ms", "the ceiling": "30s"} {
		t.Run(name+" itself is accepted", func(t *testing.T) {
			boardAgentSurface(t)
			t.Setenv("RA8CI_BOARD_AGENT_POLL_INTERVAL", value)
			err := runBoardAgent(context.Background())
			if err == nil || strings.Contains(err.Error(), refused) {
				t.Fatalf("err=%v; want %s accepted", err, value)
			}
			if !strings.Contains(err.Error(), "board-agent client") {
				t.Fatalf("err=%v; want the run stopped at its absent credentials", err)
			}
		})
	}

	// An unset interval is the one-second default and is not an error.
	t.Run("no interval named at all", func(t *testing.T) {
		boardAgentSurface(t)
		err := runBoardAgent(context.Background())
		if err == nil || strings.Contains(err.Error(), refused) {
			t.Fatalf("err=%v; want the default interval taken silently", err)
		}
	})
}

func TestTheBoardAgentRefusesAStateFileItCouldNotTrust(t *testing.T) {
	t.Run("a board name that is not one", func(t *testing.T) {
		boardAgentSurface(t)
		t.Setenv(envBoardID, "ek ra8d2")
		err := runBoardAgent(context.Background())
		if err == nil || !strings.Contains(err.Error(), "board-agent state") {
			t.Fatalf("err=%v; want the state store to refuse the board name", err)
		}
	})

	t.Run("a directory the rest of the machine can write", func(t *testing.T) {
		home := boardAgentSurface(t)
		shared := filepath.Join(home, "shared")
		if err := os.Mkdir(shared, 0o700); err != nil {
			t.Fatalf("plant shared directory: %v", err)
		}
		if err := testprivatefile.OtherUsersWritable(shared); err != nil {
			t.Fatalf("grant broad write access to state directory: %v", err)
		}
		t.Setenv(envBoardStateFile, filepath.Join(shared, "highwater.json"))
		err := runBoardAgent(context.Background())
		if err == nil || !strings.Contains(err.Error(), "board-agent state") {
			t.Fatalf("err=%v; want a group- or world-writable directory refused", err)
		}
	})

	t.Run("a state file the rest of the machine can read", func(t *testing.T) {
		home := boardAgentSurface(t)
		loose := filepath.Join(home, "loose.json")
		if err := os.WriteFile(loose, []byte("{}"), 0o600); err != nil {
			t.Fatalf("plant loose state: %v", err)
		}
		if err := testprivatefile.OtherUsersReadable(loose); err != nil {
			t.Fatalf("grant broad read access to state file: %v", err)
		}
		t.Setenv(envBoardStateFile, loose)
		err := runBoardAgent(context.Background())
		if err == nil || !strings.Contains(err.Error(), "board-agent state") {
			t.Fatalf("err=%v; want a group-readable state file refused", err)
		}
	})

	t.Run("a state file that is a symlink", func(t *testing.T) {
		home := boardAgentSurface(t)
		real := filepath.Join(home, "real.json")
		if err := os.WriteFile(real, []byte("{}"), 0o600); err != nil {
			t.Fatalf("plant state: %v", err)
		}
		link := filepath.Join(home, "link.json")
		if err := os.Symlink(real, link); err != nil {
			t.Fatalf("plant symlink: %v", err)
		}
		t.Setenv(envBoardStateFile, link)
		err := runBoardAgent(context.Background())
		if err == nil || !strings.Contains(err.Error(), "board-agent state") {
			t.Fatalf("err=%v; want a symlinked state file refused", err)
		}
	})

	t.Run("a state directory reached through a symlink", func(t *testing.T) {
		home := boardAgentSurface(t)
		real := filepath.Join(home, "state.d")
		if err := os.Mkdir(real, 0o700); err != nil {
			t.Fatalf("plant state directory: %v", err)
		}
		link := filepath.Join(home, "state-link")
		if err := os.Symlink(real, link); err != nil {
			t.Fatalf("plant directory symlink: %v", err)
		}
		t.Setenv(envBoardStateFile, filepath.Join(link, "highwater.json"))
		err := runBoardAgent(context.Background())
		if err == nil || !strings.Contains(err.Error(), "board-agent state") {
			t.Fatalf("err=%v; want a state path through a symlink refused", err)
		}
	})

	// A state file that does not exist yet is the ordinary first start, so it
	// is accepted and the run goes on to its credentials. That is what proves
	// the refusals above are about trust and not about absence.
	t.Run("a state file that is not there yet", func(t *testing.T) {
		boardAgentSurface(t)
		err := runBoardAgent(context.Background())
		if err == nil || strings.Contains(err.Error(), "board-agent state") {
			t.Fatalf("err=%v; want a first start accepted", err)
		}
		if !strings.Contains(err.Error(), "board-agent client") {
			t.Fatalf("err=%v; want the run stopped at its absent credentials", err)
		}
	})
}
