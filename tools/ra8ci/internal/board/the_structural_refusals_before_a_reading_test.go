package board

import (
	"testing"
	"time"
)

// The structural refusals Validate files before anything reads a board, and
// the two places ObserveHolderLiveness depends on them. Validate is the door
// every load and store passes through and the first thing a liveness reading
// calls, so a snapshot it refuses never becomes a sentence anybody acts on.
// Each refusal below is a shape this reducer cannot produce: a hand-edited
// row, a partial restore, or a merge of two boards' state.

func structuralAt(offset time.Duration) time.Time {
	return testEpoch.Add(offset)
}

// structuralHeld is an Active board held by agent-a under lease l-1, with the
// agent's installed generation caught up to the server's.
func structuralHeld(t *testing.T) Snapshot {
	t.Helper()
	s, err := New("board-structural")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	s, _, err = Apply(s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-1", LeaseID: "l-1", Holder: "agent-a", Class: ClassAI, Reason: "experiment", Duration: time.Hour,
	}}, structuralAt(0))
	if err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	if s.Phase != GrantPending {
		t.Fatalf("board not pending: %v", s.Phase)
	}
	s, _, err = Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: s.Generation, InstalledGeneration: s.Generation}, structuralAt(0))
	if err != nil {
		t.Fatalf("acknowledge: %v", err)
	}
	if s.Phase != Active {
		t.Fatalf("board not active: %v", s.Phase)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("fixture does not validate: %v", err)
	}
	return s
}

// structuralPending is the same board one command earlier, before the agent
// has installed the grant. It is the one live phase the install check exempts.
func structuralPending(t *testing.T) Snapshot {
	t.Helper()
	s, err := New("board-structural")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	s, _, err = Apply(s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-1", LeaseID: "l-1", Holder: "agent-a", Class: ClassAI, Reason: "experiment", Duration: time.Hour,
	}}, structuralAt(0))
	if err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	if s.Phase != GrantPending {
		t.Fatalf("board not pending: %v", s.Phase)
	}
	return s
}

// structuralEdit copies a snapshot deeply enough to hand-edit the lease
// without disturbing the fixture the other cases read.
func structuralEdit(s Snapshot) Snapshot {
	out := s
	if s.Lease != nil {
		lease := *s.Lease
		out.Lease = &lease
	}
	out.Queue = append([]Waiter(nil), s.Queue...)
	return out
}

func structuralWaiter(id, leaseID string, sequence uint64) Waiter {
	return Waiter{
		ID:       id,
		LeaseID:  leaseID,
		Holder:   "agent-b",
		Class:    ClassAI,
		Reason:   "queued work",
		Duration: 30 * time.Minute,
		Sequence: sequence,
		QueuedAt: structuralAt(time.Minute),
	}
}

// structuralRefusal asserts Validate refuses s with the given code and detail,
// and reports the detail so a caller can compare orderings.
func structuralRefusal(t *testing.T, s Snapshot, code Code, detail string) {
	t.Helper()
	err := Validate(s)
	if err == nil {
		t.Fatalf("validate admitted the snapshot")
	}
	if !IsCode(err, code) {
		t.Fatalf("wrong code: %v", err)
	}
	var boardErr *Error
	if !asStructuralError(err, &boardErr) {
		t.Fatalf("not a board error: %v", err)
	}
	if boardErr.Detail != detail {
		t.Fatalf("wrong detail: %q, want %q", boardErr.Detail, detail)
	}
}

func asStructuralError(err error, out **Error) bool {
	boardErr, ok := err.(*Error)
	if !ok {
		return false
	}
	*out = boardErr
	return true
}

func TestAnUnnamedBoardIsRefusedBeforeAnythingElseIsJudged(t *testing.T) {
	s := structuralEdit(structuralHeld(t))
	s.BoardID = ""
	structuralRefusal(t, s, InvalidArgument, "empty board ID")

	// The name is judged first, so a snapshot that is also wrong in three
	// later ways still reads as the missing name. An operator handed the
	// third refusal would go looking for a generation problem on a board
	// whose identity is what is actually missing.
	alsoBroken := structuralEdit(s)
	alsoBroken.AgentHighWater = alsoBroken.Generation + 5
	alsoBroken.Lease.Generation = alsoBroken.Generation + 2
	alsoBroken.Phase = Ready
	structuralRefusal(t, alsoBroken, InvalidArgument, "empty board ID")
}

func TestAnAgentAheadOfTheServerIsRefusedOutsideRecovery(t *testing.T) {
	held := structuralHeld(t)
	for _, phase := range []Phase{Ready, GrantPending, Active, YieldRequested, Draining, RecoveryRequired} {
		s := structuralEdit(held)
		s.Phase = phase
		s.AgentHighWater = s.Generation + 1
		structuralRefusal(t, s, Conflict, "agent high-water exceeds server generation outside quarantine")
	}
}

func TestOnlyQuarantineAndRecoveryAdmitAnAgentAheadOfTheServer(t *testing.T) {
	// The board agent installing a generation the server has no record of
	// is exactly what a restored-from-backup server looks like from the
	// board's side. Quarantined and Recovering are the two phases that
	// exist to hold that state while it is reconciled, so the refusal has
	// to stand down in them or the board could never be loaded to fix it.
	held := structuralHeld(t)
	for _, phase := range []Phase{Quarantined, Recovering} {
		s := structuralEdit(held)
		s.Phase = phase
		s.AgentHighWater = s.Generation + 7
		if err := Validate(s); err != nil {
			t.Fatalf("%s refused a reconcilable board: %v", phase, err)
		}
	}
}

func TestALeaseFromAGenerationTheBoardNeverReachedIsRefused(t *testing.T) {
	held := structuralHeld(t)
	// Judged in every phase, the recovery ones included, because that is
	// where a retained lease sits longest without anything else reading it.
	for _, phase := range []Phase{Active, RecoveryRequired, Recovering, Quarantined} {
		s := structuralEdit(held)
		s.Phase = phase
		s.Lease.Generation = s.Generation + 1
		structuralRefusal(t, s, Conflict, "lease generation exceeds board generation")
	}
}

func TestAReadyBoardRetainingALeaseIsRefused(t *testing.T) {
	// Ready means free. A board reporting itself free while holding a lease
	// tells a waiter it may take the board and tells the holder its work is
	// still authorized, and both of them are reading the same row.
	s := structuralEdit(structuralHeld(t))
	s.Phase = Ready
	s.AgentHighWater = 0
	structuralRefusal(t, s, Conflict, "ready board retains lease")
}

func TestALivePhaseWithoutItsCurrentLeaseIsRefused(t *testing.T) {
	held := structuralHeld(t)

	missing := structuralEdit(held)
	missing.Lease = nil
	structuralRefusal(t, missing, Conflict, "live phase lacks valid current lease")

	// A lease from an earlier generation is the same fault wearing a
	// plausible row: the board has moved on and the retained grant names
	// authority that was already revoked.
	stale := structuralEdit(held)
	stale.Generation = stale.Lease.Generation + 1
	structuralRefusal(t, stale, Conflict, "live phase lacks valid current lease")
}

func TestAnActiveGrantTheAgentNeverInstalledIsRefused(t *testing.T) {
	held := structuralHeld(t)
	for _, phase := range []Phase{Active, YieldRequested, Draining} {
		s := structuralEdit(held)
		s.Phase = phase
		if phase != Active {
			s.Lease.YieldRequestedAt = structuralAt(time.Minute)
		}
		s.AgentHighWater = 0
		structuralRefusal(t, s, Conflict, "active grant has not been installed by agent")
	}
}

func TestAGrantPendingBoardIsNotHeldToTheInstall(t *testing.T) {
	// Waiting for the agent to install the grant is what GrantPending is
	// for. Holding it to the install check would refuse every board in the
	// one phase where the gap is the expected state.
	s := structuralPending(t)
	if s.AgentHighWater == s.Generation {
		t.Fatalf("fixture already installed: high-water %d generation %d", s.AgentHighWater, s.Generation)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("pending grant refused: %v", err)
	}
}

func TestAPersistedQueueCannotRepeatAWaiterID(t *testing.T) {
	s := structuralEdit(structuralHeld(t))
	s.Queue = []Waiter{structuralWaiter("w-2", "l-2", 2), structuralWaiter("w-2", "l-3", 3)}
	s.NextSequence = 4
	structuralRefusal(t, s, Conflict, "duplicate waiter ID")
}

func TestAPersistedQueueCannotRepeatALeaseID(t *testing.T) {
	// Two rows sharing a lease ID are one grant as far as everything
	// downstream is concerned, so whichever is granted second hands out a
	// token the first waiter also believes is theirs.
	s := structuralEdit(structuralHeld(t))
	s.Queue = []Waiter{structuralWaiter("w-2", "l-2", 2), structuralWaiter("w-3", "l-2", 3)}
	s.NextSequence = 4
	structuralRefusal(t, s, Conflict, "duplicate queued lease ID")
}

func TestAnHonestQueueOfTwoIsStillAdmitted(t *testing.T) {
	s := structuralEdit(structuralHeld(t))
	s.Queue = []Waiter{structuralWaiter("w-2", "l-2", 2), structuralWaiter("w-3", "l-3", 3)}
	s.NextSequence = 4
	if err := Validate(s); err != nil {
		t.Fatalf("honest queue refused: %v", err)
	}
}

func TestNoLivenessReadingIsTakenOfASnapshotValidateRefuses(t *testing.T) {
	// The liveness report is the one board output written for a person to
	// act on, and every field in it is copied off the retained lease. A
	// structurally impossible board must not produce one: reporting that
	// agent-a was last seen a minute ago, off a lease whose generation the
	// board never issued, is a sentence with nothing behind it.
	held := structuralHeld(t)
	cases := []struct {
		name string
		edit func(Snapshot) Snapshot
	}{
		{"unnamed board", func(s Snapshot) Snapshot { s.BoardID = ""; return s }},
		{"lease from an unreached generation", func(s Snapshot) Snapshot {
			s.Lease.Generation = s.Generation + 1
			return s
		}},
		{"grant never installed", func(s Snapshot) Snapshot { s.AgentHighWater = 0; return s }},
		{"duplicate waiter", func(s Snapshot) Snapshot {
			s.Queue = []Waiter{structuralWaiter("w-2", "l-2", 2), structuralWaiter("w-2", "l-3", 3)}
			s.NextSequence = 4
			return s
		}},
	}
	for _, tc := range cases {
		broken := tc.edit(structuralEdit(held))
		liveness, err := ObserveHolderLiveness(broken, structuralAt(time.Minute), time.Minute)
		if err == nil {
			t.Fatalf("%s: liveness reported on a refused snapshot", tc.name)
		}
		if err.Error() != Validate(broken).Error() {
			t.Fatalf("%s: liveness reworded the refusal: %v", tc.name, err)
		}
		if liveness != (HolderLiveness{}) {
			t.Fatalf("%s: refused reading is not zero: %+v", tc.name, liveness)
		}
	}
}

func TestAnObservationBeforeTheLastBeatReportsNoSilence(t *testing.T) {
	// Clocks on two machines disagree, so an observation stamped before the
	// beat it is compared against is ordinary rather than corrupt. Reported
	// raw it would be a negative silence, which formats as "-4m0s ago" in
	// the operator line and reads as a holder that will report in the
	// future. Zero is the honest answer: nothing has been missed.
	s := structuralHeld(t)
	beat := structuralAt(5 * time.Minute)
	after, _, err := Apply(s, HolderHeartbeat{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, beat)
	if err != nil {
		t.Fatalf("heartbeat refused: %v", err)
	}

	early, err := ObserveHolderLiveness(after, structuralAt(time.Minute), time.Minute)
	if err != nil {
		t.Fatalf("liveness refused: %v", err)
	}
	if !early.Held || !early.Beat {
		t.Fatalf("beat not reported: %+v", early)
	}
	if !early.LastSeenAt.Equal(beat) {
		t.Fatalf("wrong last seen: %v", early.LastSeenAt)
	}
	if early.Silence != 0 {
		t.Fatalf("silence is not clamped: %v", early.Silence)
	}
	if early.Overdue {
		t.Fatal("a holder seen ahead of the clock is reported overdue")
	}
	if early.Explain() != "holder agent-a last reported 0s ago" {
		t.Fatalf("wrong line: %q", early.Explain())
	}

	// The clamp is exactly at the beat, and one nanosecond past it the
	// silence is real again.
	atBeat, err := ObserveHolderLiveness(after, beat, time.Minute)
	if err != nil {
		t.Fatalf("liveness refused: %v", err)
	}
	if atBeat.Silence != 0 {
		t.Fatalf("silence at the beat: %v", atBeat.Silence)
	}
	past, err := ObserveHolderLiveness(after, beat.Add(time.Nanosecond), time.Minute)
	if err != nil {
		t.Fatalf("liveness refused: %v", err)
	}
	if past.Silence != time.Nanosecond {
		t.Fatalf("silence past the beat: %v", past.Silence)
	}
}
