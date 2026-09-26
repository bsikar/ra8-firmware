package board

import (
	"testing"
	"time"
)

// yieldingBoard returns an active board whose holder has been asked to yield,
// so the retained lease carries a real YieldRequestedAt stamped by the reducer.
func yieldingBoard(t *testing.T, at time.Time, duration time.Duration) Snapshot {
	t.Helper()
	s := heldBoard(t, at, duration)
	s, _, err := Apply(s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-2", LeaseID: "l-2", Holder: "operator", Class: ClassHuman, Reason: "bench work", Duration: time.Hour,
	}}, at.Add(duration/10))
	if err != nil {
		t.Fatalf("enqueue higher waiter: %v", err)
	}
	if s.Phase != YieldRequested {
		t.Fatalf("board did not ask for a yield: %v", s.Phase)
	}
	if s.Lease.YieldRequestedAt.IsZero() {
		t.Fatal("yield request carries no time")
	}
	return s
}

func withYieldRequestAt(s Snapshot, at time.Time) Snapshot {
	lease := *s.Lease
	lease.YieldRequestedAt = at
	s.Lease = &lease
	return s
}

// The stamp the handoff clock starts from may only come from inside the lease's
// own authority: at or after the grant, strictly before expiry.
func TestARetainedYieldRequestFromBeforeTheGrantIsRefused(t *testing.T) {
	s := yieldingBoard(t, testEpoch, time.Hour)
	granted, _ := yieldRequestWindow(*s.Lease)
	for name, stamp := range map[string]time.Time{
		"one nanosecond before the grant": granted.Add(-time.Nanosecond),
		"an hour before the grant":        granted.Add(-time.Hour),
	} {
		if err := Validate(withYieldRequestAt(s, stamp)); err == nil {
			t.Fatalf("%s: a request from before the grant was accepted", name)
		}
	}
}

func TestARetainedYieldRequestAtOrAfterExpiryIsRefused(t *testing.T) {
	s := yieldingBoard(t, testEpoch, time.Hour)
	_, expiry := yieldRequestWindow(*s.Lease)
	for name, stamp := range map[string]time.Time{
		"at expiry":           expiry,
		"one nanosecond past": expiry.Add(time.Nanosecond),
		"an hour past":        expiry.Add(time.Hour),
	} {
		if err := Validate(withYieldRequestAt(s, stamp)); err == nil {
			t.Fatalf("%s: a request from outside the lease was accepted", name)
		}
	}
}

func TestAYieldRequestInsideTheLeaseIsAccepted(t *testing.T) {
	s := yieldingBoard(t, testEpoch, time.Hour)
	granted, expiry := yieldRequestWindow(*s.Lease)
	for name, stamp := range map[string]time.Time{
		"at the grant":       granted,
		"midway":             granted.Add(30 * time.Minute),
		"just before expiry": expiry.Add(-time.Nanosecond),
	} {
		if err := Validate(withYieldRequestAt(s, stamp)); err != nil {
			t.Fatalf("%s: a request from inside the lease was refused: %v", name, err)
		}
	}
}

// A lease nobody has asked to yield carries no stamp, and the rule leaves it
// alone: the phase checks already decide where a zero stamp is allowed.
func TestALeaseWithNoYieldRequestIsLeftAlone(t *testing.T) {
	s := heldBoard(t, testEpoch, time.Hour)
	if !s.Lease.YieldRequestedAt.IsZero() {
		t.Fatal("an unasked lease carries a yield request")
	}
	if err := checkYieldRequestWindow(s.Lease); err != nil {
		t.Fatalf("an unasked lease was refused: %v", err)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("an unasked board was refused: %v", err)
	}
}

// The reducer cannot produce the refused shape at the expiry end: every writer
// stamps the Apply clock while the lease is live, and an expired board falls to
// recovery with the stamp it already had.
func TestTheReducerNeverStampsAYieldRequestOutsideTheLease(t *testing.T) {
	s := heldBoard(t, testEpoch, time.Minute)
	expired, _, err := Apply(s, Tick{Actor: "server"}, s.Lease.ExpiresAt.Add(time.Second))
	if err != nil {
		t.Fatalf("expiry refused: %v", err)
	}
	if expired.Phase != RecoveryRequired {
		t.Fatalf("board did not fall to recovery: %v", expired.Phase)
	}
	if !expired.Lease.YieldRequestedAt.IsZero() {
		t.Fatalf("expiry stamped a yield request: %v", expired.Lease.YieldRequestedAt)
	}
	_, _, err = Apply(expired, RequestYield{Actor: "server", WaiterID: "w-2"}, expired.Lease.ExpiresAt.Add(time.Minute))
	if err == nil {
		t.Fatal("a yield was asked of an expired lease")
	}
}

// The refusal is about the stamp, not the phase: a lease kept as recovery
// evidence is held to the same window, which is where a bad row sits unread
// until something measures it.
func TestRecoveryEvidenceIsHeldToTheSameYieldWindow(t *testing.T) {
	s := yieldingBoard(t, testEpoch, time.Minute)
	expired, _, err := Apply(s, Tick{Actor: "server"}, s.Lease.ExpiresAt.Add(time.Second))
	if err != nil {
		t.Fatalf("expiry refused: %v", err)
	}
	if expired.Phase != RecoveryRequired {
		t.Fatalf("board did not fall to recovery: %v", expired.Phase)
	}
	if err := Validate(withYieldRequestAt(expired, expired.Lease.ExpiresAt.Add(time.Second))); err == nil {
		t.Fatal("recovery evidence carrying a post-expiry yield request was accepted")
	}
}

// An extension moves expiry later, never earlier, so a request recorded under
// the old deadline stays inside the new one.
func TestExtensionKeepsAnEarlierYieldRequestInsideTheLease(t *testing.T) {
	s := yieldingBoard(t, testEpoch, 10*time.Minute)
	asked := s.Lease.YieldRequestedAt
	s, _, err := Apply(s, Extend{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation,
		Reason: "safe wrap-up", NewExpiry: s.Lease.ExpiresAt.Add(5 * time.Minute)}, asked.Add(time.Minute))
	if err != nil {
		t.Fatalf("extend refused: %v", err)
	}
	if !s.Lease.YieldRequestedAt.Equal(asked) {
		t.Fatalf("extension moved the request: %v", s.Lease.YieldRequestedAt)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("extended lease refused: %v", err)
	}
}

// The window is what YieldSampleFor measures from, so a stamp Validate refuses
// is exactly the one that would teach the estimator a latency no holder spent.
func TestAnOutOfWindowRequestWouldMisreportTheHandoffLatency(t *testing.T) {
	s := yieldingBoard(t, testEpoch, time.Hour)
	cohort := YieldCohort{
		BoardID:         s.BoardID,
		BoardModel:      "ra8p1",
		FixtureRevision: "rev-c",
		TaskName:        "smoke",
		CatalogDigest:   "digest-1",
	}
	neutral := s.Lease.YieldRequestedAt.Add(2 * time.Minute)
	events := []Event{{Kind: LeaseReleased, At: neutral, BoardID: s.BoardID, LeaseID: s.Lease.ID}}

	honest, ok, err := YieldSampleFor(s, events, cohort, 0)
	if err != nil || !ok {
		t.Fatalf("honest sample refused: ok=%v err=%v", ok, err)
	}

	tampered := withYieldRequestAt(s, s.Lease.GrantedAt.Add(-time.Hour))
	if err := Validate(tampered); err == nil {
		t.Fatal("the tampered lease was accepted")
	}
	inflated, ok, err := YieldSampleFor(tampered, events, cohort, 0)
	if err != nil || !ok {
		t.Fatalf("tampered sample refused for another reason: ok=%v err=%v", ok, err)
	}
	if inflated.Latency() <= honest.Latency() {
		t.Fatalf("tampered stamp did not inflate the latency: %v vs %v", inflated.Latency(), honest.Latency())
	}
}
