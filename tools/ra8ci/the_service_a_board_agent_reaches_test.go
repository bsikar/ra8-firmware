// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"path/filepath"
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
	t.Setenv(envBoardStateFile, filepath.Join(t.TempDir(), "board.state"))
}

// A board agent whose context is already cancelled must come back rather than
// poll. It matters more here than in most services: the agent holds a bench,
// and a service that keeps beating after the operator asked it to stop looks
// to the plane like a holder that is still alive, so the board stays taken.
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
				// The service is reached and hands its context back. What
				// it must not do is succeed, which would report a clean
				// shutdown of a lease it never held.
				if err == nil {
					t.Fatal("a cancelled board agent reported a clean run")
				}
			case <-time.After(20 * time.Second):
				t.Fatal("a cancelled board agent kept running; it must return rather than poll")
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
