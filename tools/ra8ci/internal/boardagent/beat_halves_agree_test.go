// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

func heldLease(id string) board.Snapshot {
	lease := board.Lease{ID: id}
	return board.Snapshot{Lease: &lease}
}

func TestAnUnheldLivenessHalfDisagreesWithASnapshotThatCarriesALease(t *testing.T) {
	cases := []struct {
		name     string
		snapshot board.Snapshot
		liveness boardclient.HolderLiveness
		agree    bool
	}{
		{"both say held", heldLease("a"), boardclient.HolderLiveness{Held: true, LeaseID: "a"}, true},
		{"both say free", board.Snapshot{}, boardclient.HolderLiveness{}, true},
		{"snapshot holds, liveness does not", heldLease("a"), boardclient.HolderLiveness{}, false},
		{"liveness holds, snapshot does not", board.Snapshot{}, boardclient.HolderLiveness{Held: true, LeaseID: "a"}, true},
	}
	for _, c := range cases {
		if got := beatHalvesAgree(c.snapshot, c.liveness); got != c.agree {
			t.Fatalf("%s: halves agree = %v, want %v", c.name, got, c.agree)
		}
	}
}

// A held board whose liveness half says nobody holds it is the server
// contradicting itself about the lease this agent is beating under.
func TestReportAliveRefusesAnAnswerThatDisagreesWithItself(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		return client.state, boardclient.HolderLiveness{}, nil
	}
	if _, err := agent.ReportAlive(context.Background(), token); !errors.Is(err, boardclient.ErrInvalidRequest) {
		t.Fatalf("a liveness half saying the board is free was taken as this holder's report: %v", err)
	}
}

// The unheld half is zero throughout, so accepting one would hand the caller
// a holder with no last-seen stamp and no next-beat deadline.
func TestAnUnheldHalfNeverReachesTheCaller(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		return client.state, boardclient.HolderLiveness{Interval: time.Second}, nil
	}
	liveness, err := agent.ReportAlive(context.Background(), token)
	if err == nil {
		t.Fatal("an unheld liveness half was reported to the caller")
	}
	if liveness.Held || !liveness.LastSeenAt.IsZero() || liveness.Interval != 0 {
		t.Fatalf("a refused beat returned %+v, want the zero report", liveness)
	}
}

// The rule is asked after the snapshot has been held to the token, so a board
// that no longer records this lease still answers with the staleness the
// holder has to act on rather than with a disagreement.
func TestAReleasedBoardIsStaleRatherThanADisagreement(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		released := client.state
		released.Lease = nil
		return released, boardclient.HolderLiveness{}, nil
	}
	if _, err := agent.ReportAlive(context.Background(), token); !errors.Is(err, boardclient.ErrStaleLease) {
		t.Fatalf("a released board answered with %v, want a stale lease", err)
	}
}

// A disagreement is settled, not a transport blip: reporting again cannot
// make the server's two halves agree, so the loop ends rather than spinning.
func TestKeepAliveStopsOnAnAnswerThatDisagreesWithItself(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		return client.state, boardclient.HolderLiveness{}, nil
	}
	if err := agent.KeepAlive(context.Background(), token); !errors.Is(err, boardclient.ErrInvalidRequest) {
		t.Fatalf("the holder loop kept beating at a contradictory answer: %v", err)
	}
	if client.count() != 1 {
		t.Fatalf("holder beat %d times at a settled refusal, want 1", client.count())
	}
}

// An ordinary held answer is untouched.
func TestAHeldAnswerStillReportsTheHolderAlive(t *testing.T) {
	agent, client, token := newBeatingAgent(t)
	seen := time.Now().UTC()
	client.answer = func(int) (board.Snapshot, boardclient.HolderLiveness, error) {
		return client.state, boardclient.HolderLiveness{Held: true, LeaseID: token.LeaseID,
			Holder: "agent", Beat: true, LastSeenAt: seen, Interval: minHeartbeatInterval}, nil
	}
	liveness, err := agent.ReportAlive(context.Background(), token)
	if err != nil {
		t.Fatal(err)
	}
	if !liveness.Held || liveness.LeaseID != token.LeaseID || !liveness.LastSeenAt.Equal(seen) {
		t.Fatalf("a held answer came back as %+v", liveness)
	}
}
