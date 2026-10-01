package board

import (
	"testing"
	"time"
)

// heldBoard returns an acknowledged, active board held by one AI holder.
func heldBoard(t *testing.T, at time.Time, duration time.Duration) Snapshot {
	t.Helper()
	s, err := New("board-hb")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	s, _, err = Apply(s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-1", LeaseID: "l-1", Holder: "agent-a", Class: ClassAI, Reason: "experiment", Duration: duration,
	}}, at)
	if err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	s, _, err = Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: s.Generation, InstalledGeneration: s.Generation}, at)
	if err != nil {
		t.Fatalf("acknowledge: %v", err)
	}
	if s.Phase != Active {
		t.Fatalf("board not active: %v", s.Phase)
	}
	return s
}

func TestHeartbeatRecordsTheHolderWithoutTouchingTheDeadline(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	expiry, version := s.Lease.ExpiresAt, s.Lease.DeadlineVersion
	if !s.Lease.LastHeartbeatAt.IsZero() {
		t.Fatal("fresh grant already carries a beat")
	}

	beat := start.Add(30 * time.Second)
	after, events, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, beat)
	if err != nil {
		t.Fatalf("heartbeat refused: %v", err)
	}
	if !after.Lease.LastHeartbeatAt.Equal(beat) {
		t.Fatalf("beat not recorded: %v", after.Lease.LastHeartbeatAt)
	}
	if !after.Lease.ExpiresAt.Equal(expiry) || after.Lease.DeadlineVersion != version {
		t.Fatalf("heartbeat moved the deadline: %v v%d", after.Lease.ExpiresAt, after.Lease.DeadlineVersion)
	}
	if after.Version == s.Version {
		t.Fatal("recorded beat did not advance the snapshot version")
	}
	if len(events) != 0 {
		t.Fatalf("heartbeat audited an event: %+v", events)
	}
}

func TestHeartbeatIsNotAnExtension(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, 10*time.Minute)
	expiry := s.Lease.ExpiresAt

	now := start
	for i := 0; i < 20; i++ {
		now = now.Add(20 * time.Second)
		next, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, now)
		if err != nil {
			t.Fatalf("beat %d refused: %v", i, err)
		}
		s = next
	}
	if !s.Lease.ExpiresAt.Equal(expiry) {
		t.Fatalf("beating lengthened the lease: %v", s.Lease.ExpiresAt)
	}
	// Past expiry the holder is done beating, whatever it reports.
	if _, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, expiry.Add(time.Second)); err == nil {
		t.Fatal("expired lease accepted a beat")
	}
}

func TestSilenceDoesNotShortenAuthority(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	quiet := start.Add(45 * time.Minute)

	liveness, err := ObserveHolderLiveness(s, quiet, time.Minute)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	if !liveness.Overdue {
		t.Fatal("45 minutes of silence not reported overdue")
	}
	// Observing changes nothing, and a tick over the same silence leaves the
	// lease exactly where it was: the server waits for expiry.
	after, _, err := Apply(s, Tick{Actor: "server"}, quiet)
	if err != nil {
		t.Fatalf("tick: %v", err)
	}
	if after.Phase != Active || after.Lease == nil || !after.Lease.ExpiresAt.Equal(s.Lease.ExpiresAt) {
		t.Fatalf("silence withdrew authority: %v %+v", after.Phase, after.Lease)
	}
	if liveness.ExpiresAt != s.Lease.ExpiresAt {
		t.Fatal("liveness misreports when authority ends")
	}
}

func TestLivenessFallsBackToTheGrantAndReportsTheGrace(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)

	fresh, err := ObserveHolderLiveness(s, start.Add(time.Minute), time.Minute)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	if !fresh.Held || fresh.Beat || !fresh.LastSeenAt.Equal(s.Lease.GrantedAt) {
		t.Fatalf("grant not taken as the first observation: %+v", fresh)
	}
	if fresh.Overdue || fresh.Silence != time.Minute || fresh.Holder != "agent-a" || fresh.Class != ClassAI || fresh.LeaseID != "l-1" {
		t.Fatalf("unexpected liveness: %+v", fresh)
	}

	// Exactly at the grace is not overdue; a nanosecond past it is.
	grace := time.Duration(HeartbeatGraceBeats) * time.Minute
	at, err := ObserveHolderLiveness(s, s.Lease.GrantedAt.Add(grace), time.Minute)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	if at.Overdue {
		t.Fatal("overdue exactly at the grace")
	}
	past, err := ObserveHolderLiveness(s, s.Lease.GrantedAt.Add(grace+time.Nanosecond), time.Minute)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	if !past.Overdue {
		t.Fatal("not overdue past the grace")
	}

	beaten, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, start.Add(2*time.Minute))
	if err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	seen, err := ObserveHolderLiveness(beaten, start.Add(3*time.Minute), time.Minute)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	if !seen.Beat || seen.Silence != time.Minute || seen.Overdue {
		t.Fatalf("beat not preferred over the grant: %+v", seen)
	}
}

func TestLivenessRefusesAnUnusableIntervalAndClockAndHasNoOpinionOnAFreeBoard(t *testing.T) {
	s := heldBoard(t, testEpoch, time.Hour)
	for _, interval := range []time.Duration{0, -time.Second, MaxHeartbeatInterval + time.Nanosecond} {
		if _, err := ObserveHolderLiveness(s, testEpoch, interval); !IsCode(err, InvalidArgument) {
			t.Fatalf("interval %v accepted: %v", interval, err)
		}
	}
	if _, err := ObserveHolderLiveness(s, time.Time{}, time.Minute); !IsCode(err, InvalidArgument) {
		t.Fatalf("zero clock accepted: %v", err)
	}

	free, err := New("board-hb")
	if err != nil {
		t.Fatalf("new: %v", err)
	}
	liveness, err := ObserveHolderLiveness(free, testEpoch, time.Minute)
	if err != nil {
		t.Fatalf("free board observation failed: %v", err)
	}
	if liveness != (HolderLiveness{}) || liveness.Explain() != "board is not held" {
		t.Fatalf("free board reported a holder: %+v", liveness)
	}
}

func TestStaleHolderCannotBeatAndTheRefusalIsAudited(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	live := s.Lease.Generation

	cases := []struct {
		name    string
		command HolderHeartbeat
	}{
		{"superseded generation", HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: live + 1}},
		{"another lease", HolderHeartbeat{Actor: "agent-a", LeaseID: "l-2", Generation: live}},
		{"no generation", HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1"}},
		{"no identity", HolderHeartbeat{LeaseID: "l-1", Generation: live}},
	}
	for _, tc := range cases {
		after, events, err := Apply(s, tc.command, start.Add(time.Minute))
		if err == nil {
			t.Fatalf("%s: beat accepted", tc.name)
		}
		if !after.Lease.LastHeartbeatAt.IsZero() {
			t.Fatalf("%s: refused beat still recorded", tc.name)
		}
		if len(events) != 1 || events[0].Kind != ActionDenied {
			t.Fatalf("%s: refusal not audited: %+v", tc.name, events)
		}
	}
}

func TestBeatsOnlyMoveForwardAndOnlyWhileTheBoardIsHeld(t *testing.T) {
	start := testEpoch
	s := heldBoard(t, start, time.Hour)
	generation := s.Lease.Generation

	late := start.Add(5 * time.Minute)
	s, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: generation}, late)
	if err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	version := s.Version
	reordered, events, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: generation}, start.Add(4*time.Minute))
	if err != nil {
		t.Fatalf("out-of-order beat refused: %v", err)
	}
	if !reordered.Lease.LastHeartbeatAt.Equal(late) {
		t.Fatalf("beat moved backwards: %v", reordered.Lease.LastHeartbeatAt)
	}
	if reordered.Version != version || len(events) != 0 {
		t.Fatalf("no-op beat changed the snapshot: v%d %+v", reordered.Version, events)
	}

	// Once the board is under recovery the holder's authority is over, and a
	// beat must not make a board waiting on an operator look held.
	recovering, _, err := Apply(s, AgentUnavailable{Actor: "server", Reason: "agent lost"}, late.Add(time.Second))
	if err != nil {
		t.Fatalf("agent unavailable: %v", err)
	}
	if _, _, err := Apply(recovering, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: generation}, late.Add(2*time.Second)); err == nil {
		t.Fatal("board under recovery accepted a beat")
	}
	liveness, err := ObserveHolderLiveness(recovering, late.Add(2*time.Second), time.Minute)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	if liveness.Held {
		t.Fatalf("recovering board reported a live holder: %+v", liveness)
	}
}

func TestValidateRefusesABeatFromBeforeTheGrant(t *testing.T) {
	s := heldBoard(t, testEpoch, time.Hour)
	s.Lease.LastHeartbeatAt = s.Lease.GrantedAt.Add(-time.Nanosecond)
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("beat predating the grant accepted: %v", err)
	}
	s.Lease.LastHeartbeatAt = s.Lease.GrantedAt
	if err := Validate(s); err != nil {
		t.Fatalf("beat at the grant refused: %v", err)
	}
}
