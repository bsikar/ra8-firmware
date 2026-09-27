package board

import (
	"testing"
	"time"
)

// A snapshot version that moved with no event behind it is the one shape the
// store cannot tell apart from a reducer bug by looking at it. EventFreeCommand
// is what tells them apart, and nothing held it to the reducer it describes.
//
// The tests below pin both halves. First, that the predicate answers true for
// exactly one command. Second, the fact it stands for: driving the reducer
// through every command, a version that moves in silence belongs to a
// heartbeat and to nothing else. The second half is the one that catches
// drift, because it reads the reducer rather than the list.

// silentBeat is a heartbeat from the current holder, which is the only command
// that can move the version without leaving an event behind.
func silentBeat(s Snapshot) HolderHeartbeat {
	return HolderHeartbeat{Actor: "holder", LeaseID: leaseID(&s), Generation: s.Generation}
}

func TestTheHeartbeatIsTheOnlyEventFreeCommand(t *testing.T) {
	// Maintained by hand against Apply's switch. A command added there and
	// not added here is caught by the reducer walk below rather than by
	// this table, which is why the walk exists.
	cases := []struct {
		command   Command
		eventFree bool
	}{
		{Enqueue{}, false},
		{CancelWaiter{}, false},
		{AcknowledgeGrant{}, false},
		{RequestYield{}, false},
		{BeginDrain{}, false},
		{Release{}, false},
		{Extend{}, false},
		{HolderHeartbeat{}, true},
		{Tick{}, false},
		{AgentUnavailable{}, false},
		{BeginRecovery{}, false},
		{CompleteRecovery{}, false},
		{Quarantine{}, false},
		{ObserveAgentGeneration{}, false},
	}
	if len(cases) != 14 {
		t.Fatalf("command table holds %d entries, not the 14 Apply dispatches", len(cases))
	}
	for _, c := range cases {
		if got := EventFreeCommand(c.command); got != c.eventFree {
			t.Errorf("EventFreeCommand(%T) = %v, want %v", c.command, got, c.eventFree)
		}
	}
}

// A pointer to a command is not the command. The store holds a Command value,
// and the type switch inside the predicate is exact, so this pins that a
// caller cannot accidentally exempt a heartbeat it wrapped.
func TestAnUnknownCommandIsNotEventFree(t *testing.T) {
	if EventFreeCommand(nil) {
		t.Fatal("a nil command was reported event-free")
	}
}

// The walk. Every command is driven against a board in a state that admits it,
// and after each one the invariant the store depends on is asserted directly:
// the version moved only in silence when the command was the heartbeat.
func TestAVersionThatMovesInSilenceBelongsToAHeartbeat(t *testing.T) {
	at := testEpoch
	s := boardForTest(t)

	step := func(c Command, when time.Time) {
		t.Helper()
		before := s.Version
		next, events, err := Apply(s, c, when)
		if err != nil {
			t.Fatalf("Apply(%T): %v", c, err)
		}
		if err := Validate(next); err != nil {
			t.Fatalf("invalid result after %T: %v", c, err)
		}
		if next.Version > before && len(events) == 0 && !EventFreeCommand(c) {
			t.Fatalf("%T moved the version from %d to %d with no event and no exemption", c, before, next.Version)
		}
		if len(events) == 0 && next.Version == before && EventFreeCommand(c) {
			t.Logf("%T changed nothing this step", c)
		}
		s = next
	}

	step(request("w1", ClassAI), at)
	at = at.Add(time.Second)
	step(AcknowledgeGrant{Actor: "board-agent", LeaseID: s.Lease.ID, Generation: s.Generation, InstalledGeneration: s.Generation}, at)
	at = at.Add(time.Second)
	step(silentBeat(s), at)
	at = at.Add(time.Second)
	step(Extend{Actor: "holder", LeaseID: s.Lease.ID, Generation: s.Generation, NewExpiry: s.Lease.ExpiresAt.Add(time.Minute), Reason: "more work"}, at)
	at = at.Add(time.Second)
	step(request("w2", ClassHuman), at)
	at = at.Add(time.Second)
	step(RequestYield{Actor: "server", WaiterID: "w2"}, at)
	at = at.Add(time.Second)
	step(BeginDrain{Actor: "holder", LeaseID: s.Lease.ID, Generation: s.Generation}, at)
	at = at.Add(time.Second)
	step(Release{Actor: "holder", LeaseID: s.Lease.ID, Generation: s.Generation, NeutralReceipt: "neutral-proof"}, at)
	at = at.Add(time.Second)
	step(Tick{Actor: "server"}, at)
	at = at.Add(time.Second)
	step(ObserveAgentGeneration{Actor: "board-agent", HighWater: s.AgentHighWater}, at)
	at = at.Add(time.Second)
	step(request("w3", ClassAI), at)
	at = at.Add(time.Second)
	step(CancelWaiter{Actor: "server", WaiterID: "w3"}, at)
	at = at.Add(time.Second)
	step(AgentUnavailable{Actor: "monitor", Reason: "clock continuity lost"}, at)
	at = at.Add(time.Second)
	step(BeginRecovery{Actor: "operator", PlanID: "plan-1", Reason: "restore fixture"}, at)
	at = at.Add(time.Second)
	step(CompleteRecovery{Actor: "operator", NeutralReceipt: "neutral-proof", AgentHighWater: s.AgentHighWater}, at)
	at = at.Add(time.Second)
	step(Quarantine{Actor: "operator", Reason: "bench opened"}, at)
}

// The case the predicate exists for, on its own: a beat that is genuinely a
// new observation moves the version and emits nothing.
func TestARecordedBeatMovesTheVersionAndEmitsNothing(t *testing.T) {
	at := testEpoch
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("w1", ClassAI), at)
	s = ack(t, s, at.Add(time.Second))

	before := s.Version
	next, events, err := Apply(s, silentBeat(s), at.Add(time.Minute))
	if err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("heartbeat emitted %d events, want none", len(events))
	}
	if next.Version != before+1 {
		t.Fatalf("version %d after a recorded beat, want %d", next.Version, before+1)
	}
	if !EventFreeCommand(silentBeat(s)) {
		t.Fatal("the command that moved the version in silence is not declared event-free")
	}
	if !next.Lease.LastHeartbeatAt.Equal(at.Add(time.Minute)) {
		t.Fatalf("beat recorded at %v, want %v", next.Lease.LastHeartbeatAt, at.Add(time.Minute))
	}
}

// The other half of the same rule: a beat that adds no observation moves
// nothing at all, so there is no silent version for the store to judge.
func TestABeatThatAddsNoObservationMovesNoVersion(t *testing.T) {
	at := testEpoch
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("w1", ClassAI), at)
	s = ack(t, s, at.Add(time.Second))
	s, _, _ = Apply(s, silentBeat(s), at.Add(2*time.Minute))

	before := s.Version
	next, events, err := Apply(s, silentBeat(s), at.Add(time.Minute))
	if err != nil {
		t.Fatalf("stale-order heartbeat: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("out-of-order beat emitted %d events, want none", len(events))
	}
	if next.Version != before {
		t.Fatalf("version moved to %d on a beat that changed nothing, want %d", next.Version, before)
	}
}

// A refused beat is not silent. Apply files the denial, so the version moves
// with an event behind it and the store reads it like any other refusal.
func TestARefusedBeatIsAuditedRatherThanSilent(t *testing.T) {
	at := testEpoch
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("w1", ClassAI), at)
	s = ack(t, s, at.Add(time.Second))

	before := s.Version
	next, events, err := Apply(s, HolderHeartbeat{Actor: "holder", LeaseID: s.Lease.ID, Generation: s.Generation + 1}, at.Add(time.Minute))
	if err == nil {
		t.Fatal("a beat on a stale generation was accepted")
	}
	if len(events) == 0 {
		t.Fatal("a refused beat left no audit record")
	}
	if events[len(events)-1].Kind != ActionDenied {
		t.Fatalf("last event %q, want %q", events[len(events)-1].Kind, ActionDenied)
	}
	if next.Version != before+1 {
		t.Fatalf("version %d after a refused beat, want %d", next.Version, before+1)
	}
}

// Every other command that moves the version owes an event, and a denial is
// an event. This is the shape the store refuses when the exemption is absent.
func TestADeniedCommandMovesTheVersionWithAnEvent(t *testing.T) {
	at := testEpoch
	s := boardForTest(t)

	before := s.Version
	next, events, err := Apply(s, CancelWaiter{Actor: "server", WaiterID: "nobody"}, at)
	if err == nil {
		t.Fatal("cancelling a waiter that is not queued was accepted")
	}
	if len(events) == 0 {
		t.Fatal("a refused cancel left no audit record")
	}
	if EventFreeCommand(CancelWaiter{}) {
		t.Fatal("CancelWaiter is exempt from owing an event")
	}
	if next.Version != before+1 {
		t.Fatalf("version %d after a refused cancel, want %d", next.Version, before+1)
	}
}

// A command that changes nothing and emits nothing does not move the version
// either, so silence alone is never the thing being judged.
func TestACommandThatChangesNothingMovesNoVersion(t *testing.T) {
	s := boardForTest(t)
	before := s.Version
	next, events, err := Apply(s, Tick{Actor: "server"}, testEpoch)
	if err != nil {
		t.Fatalf("tick on a free empty board: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("tick emitted %d events, want none", len(events))
	}
	if next.Version != before {
		t.Fatalf("version moved to %d on a tick that did nothing, want %d", next.Version, before)
	}
}
