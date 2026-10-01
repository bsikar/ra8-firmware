package board

import (
	"strings"
	"testing"
	"time"
)

// Explain is the one liveness output a person reads rather than a caller
// branches on, and its two held readings had no test at all. The claim it
// makes is the whole reason the report exists: silence is evidence to report,
// never authority withdrawn, so the overdue line has to say the holder is
// still the holder. A line that reads like an expiry would have an operator
// recovering a board whose lease is running perfectly well.

// observedAt reads the liveness of s at the given moment on a one-minute
// reporting interval, which puts the grace at three minutes.
func observedAt(t *testing.T, s Snapshot, at time.Time) HolderLiveness {
	t.Helper()
	liveness, err := ObserveHolderLiveness(s, at, time.Minute)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	return liveness
}

// heldForLine is an acknowledged board held by owner-w1 from testEpoch.
func heldForLine(t *testing.T) Snapshot {
	t.Helper()
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("w1", ClassAI), testEpoch)
	return ack(t, s, testEpoch)
}

func TestTheHeldLineNamesTheHolderAndHowLongItHasBeenQuiet(t *testing.T) {
	s := heldForLine(t)
	line := observedAt(t, s, testEpoch.Add(time.Minute)).Explain()
	if line != "holder owner-w1 last reported 1m0s ago" {
		t.Fatalf("held line reads %q", line)
	}
}

func TestTheOverdueLineSaysTheAuthorityStillRuns(t *testing.T) {
	s := heldForLine(t)
	liveness := observedAt(t, s, testEpoch.Add(10*time.Minute))
	if !liveness.Overdue {
		t.Fatal("ten minutes of silence on a one-minute interval is not overdue")
	}
	line := liveness.Explain()
	if line != "holder owner-w1 has not reported for 10m0s; authority still runs to expiry" {
		t.Fatalf("overdue line reads %q", line)
	}
}

// The claim, stated as the test that would fail if the wording ever drifted
// toward an expiry: the lease has not ended, and the line must not say it has.
func TestNoLivenessLineStatesTheLeaseHasEnded(t *testing.T) {
	s := heldForLine(t)
	for _, at := range []time.Time{
		testEpoch,
		testEpoch.Add(time.Minute),
		testEpoch.Add(3 * time.Minute),
		testEpoch.Add(10 * time.Minute),
	} {
		line := observedAt(t, s, at).Explain()
		for _, ended := range []string{"expired", "expiry has", "lease ended", "no longer", "lost the board", "revoked"} {
			if strings.Contains(line, ended) {
				t.Fatalf("liveness line %q reads as an ended lease (%q)", line, ended)
			}
		}
		if !strings.Contains(line, "owner-w1") {
			t.Fatalf("liveness line %q does not name the holder", line)
		}
	}
}

// Explain switches on exactly the boundary Overdue reports, so the line a
// person reads and the flag a caller branches on can never disagree.
func TestTheLineTurnsOverdueExactlyWhereTheFlagDoes(t *testing.T) {
	s := heldForLine(t)
	grace := time.Duration(HeartbeatGraceBeats) * time.Minute

	atGrace := observedAt(t, s, s.Lease.GrantedAt.Add(grace))
	if atGrace.Overdue {
		t.Fatal("overdue exactly at the grace")
	}
	if !strings.HasPrefix(atGrace.Explain(), "holder owner-w1 last reported ") {
		t.Fatalf("line at the grace reads %q", atGrace.Explain())
	}

	pastGrace := observedAt(t, s, s.Lease.GrantedAt.Add(grace+time.Nanosecond))
	if !pastGrace.Overdue {
		t.Fatal("not overdue past the grace")
	}
	if !strings.HasPrefix(pastGrace.Explain(), "holder owner-w1 has not reported for ") {
		t.Fatalf("line past the grace reads %q", pastGrace.Explain())
	}
}

// A beat resets what the line says, because the report is about the last
// observation rather than about the grant.
func TestABeatMovesTheLineBackToTheBeat(t *testing.T) {
	s := heldForLine(t)
	beaten, _, err := Apply(s, HolderHeartbeat{Actor: "owner-w1", LeaseID: s.Lease.ID, Generation: s.Generation}, testEpoch.Add(9*time.Minute))
	if err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	liveness := observedAt(t, beaten, testEpoch.Add(10*time.Minute))
	if !liveness.Beat {
		t.Fatal("the beat was not preferred over the grant")
	}
	if line := liveness.Explain(); line != "holder owner-w1 last reported 1m0s ago" {
		t.Fatalf("line after a beat reads %q", line)
	}
}

// Every phase that has a holder produces a held line. GrantPending counts:
// the board was granted, and the grant is itself an observation.
func TestEveryLivePhaseReadsAsHeld(t *testing.T) {
	pending := boardForTest(t)
	pending, _ = applyForTest(t, pending, request("w1", ClassAI), testEpoch)
	if pending.Phase != GrantPending {
		t.Fatalf("phase %q, want %q", pending.Phase, GrantPending)
	}

	active := ack(t, pending, testEpoch)
	asked, _ := applyForTest(t, active, request("w2", ClassHuman), testEpoch)
	if asked.Phase != YieldRequested {
		t.Fatalf("phase %q, want %q", asked.Phase, YieldRequested)
	}
	draining, _ := applyForTest(t, asked, BeginDrain{Actor: "owner-w1", LeaseID: asked.Lease.ID, Generation: asked.Generation}, testEpoch)
	if draining.Phase != Draining {
		t.Fatalf("phase %q, want %q", draining.Phase, Draining)
	}

	for _, s := range []Snapshot{pending, active, asked, draining} {
		liveness := observedAt(t, s, testEpoch.Add(time.Minute))
		if !liveness.Held {
			t.Fatalf("phase %q reports no holder", s.Phase)
		}
		if !strings.Contains(liveness.Explain(), "owner-w1") {
			t.Fatalf("phase %q line %q does not name the holder", s.Phase, liveness.Explain())
		}
	}
}

// The sharp end of the same rule. The recovery phases retain the old lease as
// evidence, so a holder is still written down, and liveness deliberately says
// nothing about it: there is no live holder to be silent. The line must not
// invite an operator to chase one.
func TestTheRecoveryPhasesReadAsNotHeldEvenHoldingALease(t *testing.T) {
	active := ack(t, func() Snapshot {
		s := boardForTest(t)
		s, _ = applyForTest(t, s, request("w1", ClassAI), testEpoch)
		return s
	}(), testEpoch)

	needed, _ := applyForTest(t, active, AgentUnavailable{Actor: "monitor", Reason: "clock continuity lost"}, testEpoch)
	recovering, _ := applyForTest(t, needed, BeginRecovery{Actor: "operator", PlanID: "plan-1", Reason: "restore fixture"}, testEpoch)
	quarantined, _ := applyForTest(t, active, Quarantine{Actor: "operator", Reason: "bench opened"}, testEpoch)

	for _, s := range []Snapshot{needed, recovering, quarantined} {
		if s.Lease == nil {
			t.Fatalf("phase %q dropped the lease it retains as evidence", s.Phase)
		}
		liveness := observedAt(t, s, testEpoch.Add(time.Minute))
		if liveness != (HolderLiveness{}) {
			t.Fatalf("phase %q reported a holder: %+v", s.Phase, liveness)
		}
		if line := liveness.Explain(); line != "board is not held" {
			t.Fatalf("phase %q line reads %q", s.Phase, line)
		}
	}
}
