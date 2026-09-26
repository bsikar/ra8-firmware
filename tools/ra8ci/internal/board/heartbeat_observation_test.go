package board

import (
	"testing"
	"time"
)

// A beat stamped before the grant is not a newer observation than the grant
// itself, so it records nothing and leaves the board usable.
func TestABeatFromBeforeTheGrantIsNotRecorded(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	for name, beat := range map[string]time.Time{
		"a minute early": s.Lease.GrantedAt.Add(-time.Minute),
		"a day early":    s.Lease.GrantedAt.Add(-24 * time.Hour),
		"at the grant":   s.Lease.GrantedAt,
	} {
		after, events, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, beat)
		if err != nil {
			t.Fatalf("%s: beat refused outright: %v", name, err)
		}
		if len(events) != 0 {
			t.Fatalf("%s: beat emitted events: %+v", name, events)
		}
		if !after.Lease.LastHeartbeatAt.IsZero() {
			t.Fatalf("%s: the beat reached the lease: %v", name, after.Lease.LastHeartbeatAt)
		}
		if err := Validate(after); err != nil {
			t.Fatalf("%s: the reducer left a snapshot it refuses: %v", name, err)
		}
	}
}

func TestABeatAfterTheGrantIsStillRecorded(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	beat := s.Lease.GrantedAt.Add(time.Minute)
	after, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, beat)
	if err != nil {
		t.Fatalf("beat refused: %v", err)
	}
	if !after.Lease.LastHeartbeatAt.Equal(beat) {
		t.Fatalf("beat not recorded: %v", after.Lease.LastHeartbeatAt)
	}
}

// The consequence the rule exists to prevent: Validate refuses a lease
// carrying a pre-grant beat, and Apply validates the snapshot it is handed, so
// recording one would fail every later command against that board.
func TestAPreGrantBeatWouldRefuseEveryLaterCommand(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	lease := *s.Lease
	lease.LastHeartbeatAt = lease.GrantedAt.Add(-time.Minute)
	wedged := s
	wedged.Lease = &lease
	if err := Validate(wedged); err == nil {
		t.Fatal("a lease carrying a pre-grant beat was accepted")
	}
	if _, _, err := Apply(wedged, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: wedged.Generation}, start.Add(time.Minute)); err == nil {
		t.Fatal("a board carrying a pre-grant beat still accepted a command")
	}
}

// Whatever the clock says, the reducer must never leave a beat outside the
// window Validate holds a retained lease to.
func TestTheReducerNeverRecordsABeatValidateRefuses(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	for _, offset := range []time.Duration{-time.Hour, -time.Nanosecond, 0, time.Nanosecond, time.Minute, 59 * time.Minute} {
		after, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, s.Lease.GrantedAt.Add(offset))
		if err != nil {
			continue
		}
		if err := Validate(after); err != nil {
			t.Fatalf("offset %v produced a snapshot the board refuses: %v", offset, err)
		}
	}
}

// The existing out-of-order rule is unchanged: a beat older than one already
// recorded still leaves the later observation standing.
func TestAnOlderBeatStillLeavesTheLaterObservationStanding(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	late := s.Lease.GrantedAt.Add(10 * time.Minute)
	s, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, late)
	if err != nil {
		t.Fatalf("beat refused: %v", err)
	}
	after, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, late.Add(-5*time.Minute))
	if err != nil {
		t.Fatalf("reordered beat refused: %v", err)
	}
	if !after.Lease.LastHeartbeatAt.Equal(late) {
		t.Fatalf("an older beat moved the last-seen stamp: %v", after.Lease.LastHeartbeatAt)
	}
}

// The liveness report is the reader this rule protects: until a beat lands
// after the grant, the grant is what the holder was last seen by.
func TestTheGrantStaysTheObservationUntilARealBeatLands(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	after, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, s.Lease.GrantedAt.Add(-time.Minute))
	if err != nil {
		t.Fatalf("beat refused: %v", err)
	}
	liveness, err := ObserveHolderLiveness(after, s.Lease.GrantedAt.Add(time.Minute), time.Minute)
	if err != nil {
		t.Fatalf("liveness refused: %v", err)
	}
	if liveness.Beat || !liveness.LastSeenAt.Equal(s.Lease.GrantedAt) || liveness.Silence != time.Minute {
		t.Fatalf("liveness read a beat that was never recorded: %+v", liveness)
	}
}
