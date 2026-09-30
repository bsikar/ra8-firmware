// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// bindBoardAgentEnvironment gives the service mode everything it asks for: a
// sound authority and identity under the board-agent surface, a board name, a
// state file in a private directory. Each case below then spoils nothing, so
// what it observes is the service itself rather than a refusal on the way in.
//
// the_board_agent_refuses_before_it_holds_test.go holds the refusals; this
// file holds the far side of them, which no case reached before.
func bindBoardAgentEnvironment(t *testing.T, boardID string) {
	t.Helper()
	material := mintReportMaterial(t)
	t.Setenv(envServerURL, "https://ra8ci.example:8443")
	t.Setenv(envServerCA, material.caPath)
	t.Setenv(roleBoardAgent.certEnv, material.certPath)
	t.Setenv(roleBoardAgent.keyEnv, material.keyPath)
	t.Setenv(envBoardID, boardID)
	t.Setenv(envBoardStateFile, filepath.Join(boardAgentStateDirectory(t), "board.state"))
}

// boardAgentStateDirectory is a directory the board-agent state policy will
// actually accept, and it has to be made by hand: t.TempDir() creates its
// directories 0o777 before umask, which leaves them group- and world-readable,
// and the state store refuses exactly that. Using t.TempDir() directly is why
// nothing here reached the service before; the run stopped at "board-agent
// state: unsafe board-agent state file" and every case still passed, because
// each was only asking for some refusal rather than for this one.
func boardAgentStateDirectory(t *testing.T) string {
	t.Helper()
	directory := filepath.Join(t.TempDir(), "state.d")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatalf("plant a private state directory: %v", err)
	}
	return directory
}

// A board agent whose context is already cancelled must come back rather than
// poll. It matters more here than in most services: the agent holds a bench,
// and a service that keeps beating after the operator asked it to stop looks
// to the plane like a holder that is still alive, so the board stays taken.
//
// Cancellation is a clean stop, so the command answers nothing. That is the
// service's own contract rather than this test's opinion: Agent.Run treats a
// cancelled context as the operator's own shutdown and returns nil both where
// a reconcile fails under it and where the tick loop observes it. An error
// here would report a fault where somebody pressed stop.
func TestABoardAgentStopsWhenItsContextIsAlreadyCancelled(t *testing.T) {
	for name, interval := range map[string]string{
		"the default interval":  "",
		"the shortest accepted": "250ms",
		"the longest accepted":  "30s",
	} {
		t.Run(name, func(t *testing.T) {
			bindBoardAgentEnvironment(t, "ek-ra8d2")
			t.Setenv("RA8CI_BOARD_AGENT_POLL_INTERVAL", interval)

			ctx, cancel := context.WithCancel(context.Background())
			cancel()

			finished := make(chan error, 1)
			go func() { finished <- runBoardAgent(ctx) }()
			select {
			case err := <-finished:
				if err != nil {
					t.Fatalf("a cancelled board agent answered %v, want a clean stop", err)
				}
			case <-time.After(20 * time.Second):
				t.Fatal("a cancelled board agent kept running; it must return rather than poll")
			}
		})
	}
}

// The plane being unreachable does not change that answer. The agent is
// started against an address nothing answers on, so the only way it can come
// back clean is by reading its own cancellation first, which is what an
// operator stopping a service on a bench with no network expects.
func TestACancelledBoardAgentStopsCleanlyWithNoPlaneToReach(t *testing.T) {
	bindBoardAgentEnvironment(t, "ek-ra8d2")
	t.Setenv(envServerURL, "https://127.0.0.1:1/")

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	finished := make(chan error, 1)
	go func() { finished <- runBoardAgent(ctx) }()
	select {
	case err := <-finished:
		if err != nil {
			t.Fatalf("the agent answered %v, want its cancellation read before the plane", err)
		}
	case <-time.After(20 * time.Second):
		t.Fatal("the agent kept running against an unreachable plane")
	}
}

// A board name the state store will not keep is refused before any identity is
// loaded, and the refusal names the state rather than the credential. The
// board-agent surface accepts a wider vocabulary than the store does, so this
// is the one place the two disagree and the operator has to be told which of
// them objected.
func TestABoardAgentRefusesABoardNameItsStateStoreWillNotKeep(t *testing.T) {
	for _, boardID := range []string{"ek ra8d2", "ek/ra8d2", "ek:ra8d2", strings.Repeat("b", 129)} {
		t.Run(boardID, func(t *testing.T) {
			bindBoardAgentEnvironment(t, boardID)

			err := runBoardAgent(context.Background())
			if err == nil {
				t.Fatal("the board name was accepted")
			}
			if !strings.Contains(err.Error(), "board-agent") {
				t.Fatalf("refusal = %q, want the board-agent surface named", err)
			}
		})
	}
}

// The longest interval the service will take is 30s and the shortest is
// 250ms. Both bounds are exact, and a value outside them is refused before
// any state file is opened or any identity is loaded, so an operator who
// mistyped the interval is told about the interval.
func TestABoardAgentHoldsItsPollIntervalToItsBounds(t *testing.T) {
	const refusal = "RA8CI_BOARD_AGENT_POLL_INTERVAL must be between 250ms and 30s"
	for name, interval := range map[string]string{
		"a shade under the floor":   "249ms",
		"a shade over the ceiling":  "30001ms",
		"far over the ceiling":      "1h",
		"zero":                      "0s",
		"negative":                  "-1s",
		"a duration with no unit":   "5",
		"words rather than a value": "often",
	} {
		t.Run(name, func(t *testing.T) {
			bindBoardAgentEnvironment(t, "ek-ra8d2")
			t.Setenv("RA8CI_BOARD_AGENT_POLL_INTERVAL", interval)

			err := runBoardAgent(context.Background())
			if err == nil {
				t.Fatal("the interval was accepted")
			}
			if err.Error() != refusal {
				t.Fatalf("refusal = %q, want %q", err, refusal)
			}
		})
	}
}
