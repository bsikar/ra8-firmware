package board

import (
	"testing"
	"time"
)

// deadlineLease is an honest granted lease: expiry exactly where grantNext put
// it, deadline version 1, nothing extended.
func deadlineLease(now time.Time) *Lease {
	return &Lease{
		ID: "lease-held", WaiterID: "waiter-held", Holder: "ci", Class: ClassCI,
		Reason: "integration run", Generation: 3, GrantedAt: now.Add(-20 * time.Minute),
		ExpiresAt: now.Add(10 * time.Minute), RequestedDuration: 30 * time.Minute,
		DeadlineVersion: 1,
	}
}

func deadlineSnapshot(now time.Time) Snapshot {
	return Snapshot{
		BoardID:        "board-1",
		Phase:          Active,
		Generation:     3,
		AgentHighWater: 3,
		Version:        9,
		NextSequence:   1,
		Lease:          deadlineLease(now),
	}
}

func TestGrantedLeaseExpiryIsAccepted(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	if err := checkDeadlineMatchesItsExtensions(deadlineLease(now)); err != nil {
		t.Fatalf("honest granted lease refused: %v", err)
	}
	if err := checkDeadlineMatchesItsExtensions(nil); err != nil {
		t.Fatalf("nil lease refused: %v", err)
	}
}

func TestDeadlineEarlierThanTheGrantIsRefusedAtEveryVersion(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	for _, version := range []uint64{1, 2, 7, 4096} {
		lease := deadlineLease(now)
		lease.DeadlineVersion = version
		lease.ExpiresAt = lease.ExpiresAt.Add(-time.Nanosecond)
		err := checkDeadlineMatchesItsExtensions(lease)
		if !IsCode(err, Conflict) {
			t.Fatalf("version %d: shortened deadline accepted: %v", version, err)
		}
		if got := err.Error(); got != "conflict: retained lease expires before the duration its grant issued" {
			t.Fatalf("version %d: wrong detail %q", version, got)
		}
	}
}

func TestDeadlineLaterThanTheGrantNeedsAnExtensionOnRecord(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	longer := []time.Duration{time.Nanosecond, time.Second, 5 * time.Minute, 29 * time.Minute}
	for _, extra := range longer {
		lease := deadlineLease(now)
		lease.ExpiresAt = lease.ExpiresAt.Add(extra)
		err := checkDeadlineMatchesItsExtensions(lease)
		if !IsCode(err, Conflict) {
			t.Fatalf("+%s at version 1 accepted: %v", extra, err)
		}
		if got := err.Error(); got != "conflict: retained lease outlives its grant with no extension on record" {
			t.Fatalf("+%s: wrong detail %q", extra, got)
		}
		lease.DeadlineVersion = 2
		if err := checkDeadlineMatchesItsExtensions(lease); err != nil {
			t.Fatalf("+%s at version 2 refused: %v", extra, err)
		}
	}
}

// The boundary is exact on both sides of the granted expiry: one nanosecond
// short is a shortened deadline, the instant itself is the honest one, and one
// nanosecond long is unaccounted time.
func TestGrantedExpiryBoundaryIsExact(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	granted := grantedExpiry(*deadlineLease(now))
	for _, tc := range []struct {
		name    string
		at      time.Time
		refused bool
	}{
		{"one nanosecond short", granted.Add(-time.Nanosecond), true},
		{"exactly the granted expiry", granted, false},
		{"one nanosecond long", granted.Add(time.Nanosecond), true},
	} {
		lease := deadlineLease(now)
		lease.ExpiresAt = tc.at
		err := checkDeadlineMatchesItsExtensions(lease)
		if tc.refused != (err != nil) {
			t.Fatalf("%s: refused=%v err=%v", tc.name, err != nil, err)
		}
	}
}

// grantedExpiry must stay the arithmetic grantNext actually performs, so a
// lease built by the reducer itself always sits on the boundary.
func TestGrantNextLandsExactlyOnTheGrantedExpiry(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	for _, d := range []time.Duration{time.Minute, 17 * time.Minute, time.Hour} {
		before, err := New("board-1")
		if err != nil {
			t.Fatalf("new board: %v", err)
		}
		after, _, err := Apply(before, Enqueue{Actor: "server", Waiter: Waiter{
			ID: "w-1", LeaseID: "l-1", Holder: "ci", Class: ClassCI,
			Reason: "integration run", Duration: d,
		}}, now)
		if err != nil {
			t.Fatalf("enqueue %s: %v", d, err)
		}
		if after.Lease == nil {
			t.Fatalf("enqueue %s granted nothing", d)
		}
		if after.Lease.ExpiresAt != grantedExpiry(*after.Lease) {
			t.Fatalf("%s: expiry %s is not the granted expiry %s", d, after.Lease.ExpiresAt, grantedExpiry(*after.Lease))
		}
		if after.Lease.DeadlineVersion != 1 {
			t.Fatalf("%s: deadline version %d", d, after.Lease.DeadlineVersion)
		}
		if err := checkDeadlineMatchesItsExtensions(after.Lease); err != nil {
			t.Fatalf("%s: freshly granted lease refused: %v", d, err)
		}
	}
}

// Every extension the reducer performs pays a version for the time it adds, so
// a lease walked through extend stays acceptable at each step.
func TestEveryExtensionCarriesItsOwnVersion(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	snapshot := deadlineSnapshot(now)
	version := snapshot.Lease.DeadlineVersion
	expiry := snapshot.Lease.ExpiresAt
	for step := 1; step <= 4; step++ {
		expiry = expiry.Add(2 * time.Minute)
		after, _, err := Apply(snapshot, Extend{
			Actor: "ci", LeaseID: "lease-held", Generation: 3,
			NewExpiry: expiry, Reason: "flash phase overran",
		}, now)
		if err != nil {
			t.Fatalf("extend %d: %v", step, err)
		}
		if after.Lease.DeadlineVersion != version+1 {
			t.Fatalf("extend %d: version %d after %d", step, after.Lease.DeadlineVersion, version)
		}
		if err := checkDeadlineMatchesItsExtensions(after.Lease); err != nil {
			t.Fatalf("extend %d: extended lease refused: %v", step, err)
		}
		version = after.Lease.DeadlineVersion
		snapshot = after
	}
}

// The door is Validate, so an unaccounted deadline is refused wherever a
// snapshot is loaded or applied, including the recovery phases that retain the
// lease purely as evidence.
func TestValidateRefusesAnUnaccountedDeadlineInEveryPhase(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	phases := []Phase{Active, YieldRequested, Draining, RecoveryRequired, Recovering, Quarantined}
	for _, phase := range phases {
		for _, shift := range []time.Duration{-time.Minute, time.Minute} {
			s := deadlineSnapshot(now)
			s.Phase = phase
			if phase == YieldRequested || phase == Draining {
				s.Lease.YieldRequestedAt = now.Add(-time.Minute)
			}
			s.Lease.ExpiresAt = s.Lease.ExpiresAt.Add(shift)
			if err := Validate(s); !IsCode(err, Conflict) {
				t.Fatalf("phase %s shift %s: %v", phase, shift, err)
			}
		}
		s := deadlineSnapshot(now)
		s.Phase = phase
		if phase == YieldRequested || phase == Draining {
			s.Lease.YieldRequestedAt = now.Add(-time.Minute)
		}
		if err := Validate(s); err != nil {
			t.Fatalf("phase %s: honest lease refused: %v", phase, err)
		}
	}
}

// Apply refuses the whole transition on an unaccounted deadline rather than
// transitioning from it, and leaves the caller's snapshot untouched.
func TestApplyRefusesAnUnaccountedDeadlineBeforeAnyTransition(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	before := deadlineSnapshot(now)
	before.Lease.ExpiresAt = before.Lease.ExpiresAt.Add(20 * time.Minute)
	after, events, err := Apply(before, Release{
		Actor: "ci", LeaseID: "lease-held", Generation: 3, NeutralReceipt: "receipt-1",
	}, now)
	if !IsCode(err, Conflict) {
		t.Fatalf("release from an unaccounted deadline: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("events emitted: %+v", events)
	}
	if after.Lease == nil || after.Version != before.Version {
		t.Fatalf("snapshot mutated: %+v", after)
	}
}

// The existing class-ceiling check is the only other bound on ExpiresAt, and it
// is far too loose to catch this: an AI lease asked for ten minutes can sit at
// fifty-five without tripping it, which is the whole reason for the rule.
func TestClassCeilingDoesNotCatchUnaccountedTime(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	s := deadlineSnapshot(now)
	s.Lease.Class = ClassAI
	s.Lease.RequestedDuration = 10 * time.Minute
	s.Lease.GrantedAt = now
	s.Lease.ExpiresAt = now.Add(55 * time.Minute)
	if s.Lease.ExpiresAt.After(s.Lease.GrantedAt.Add(classCeiling(ClassAI))) {
		t.Fatal("fixture no longer sits under the class ceiling")
	}
	if err := checkDeadlineMatchesItsExtensions(s.Lease); !IsCode(err, Conflict) {
		t.Fatalf("forty-five unaccounted minutes under the ceiling: %v", err)
	}
}

// Fail closed and stay refused: re-reading the same bad row must not drift into
// acceptance, and the refusal is not silently repaired on the way through.
func TestRefusalIsStableAcrossReads(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	s := deadlineSnapshot(now)
	s.Lease.ExpiresAt = s.Lease.ExpiresAt.Add(time.Hour)
	want := s.Lease.ExpiresAt
	for pass := 1; pass <= 3; pass++ {
		if err := Validate(s); !IsCode(err, Conflict) {
			t.Fatalf("pass %d: %v", pass, err)
		}
		if s.Lease.ExpiresAt != want {
			t.Fatalf("pass %d: expiry rewritten to %s", pass, s.Lease.ExpiresAt)
		}
	}
}
