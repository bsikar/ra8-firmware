package board

import (
	"math"
	"strconv"
	"sync"
	"testing"
	"time"
)

var testEpoch = time.Date(2026, 9, 22, 12, 0, 0, 0, time.UTC)

func boardForTest(t *testing.T) Snapshot {
	t.Helper()
	s, err := New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func applyForTest(t *testing.T, s Snapshot, c Command, at time.Time) (Snapshot, []Event) {
	t.Helper()
	next, events, err := Apply(s, c, at)
	if err != nil {
		t.Fatalf("Apply(%T): %v", c, err)
	}
	if err := Validate(next); err != nil {
		t.Fatalf("invalid result after %T: %v", c, err)
	}
	return next, events
}

func request(id string, class Class) Enqueue {
	return Enqueue{Actor: "server", Waiter: Waiter{
		ID: id, LeaseID: "lease-" + id, Holder: "owner-" + id,
		Class: class, Reason: "test", Duration: 20 * time.Minute,
	}}
}

func ack(t *testing.T, s Snapshot, at time.Time) Snapshot {
	t.Helper()
	if s.Lease == nil {
		t.Fatal("missing pending lease")
	}
	next, _ := applyForTest(t, s, AcknowledgeGrant{
		Actor: "board-agent", LeaseID: s.Lease.ID,
		Generation: s.Generation, InstalledGeneration: s.Generation,
	}, at)
	return next
}

func releaseNeutral(t *testing.T, s Snapshot, at time.Time) Snapshot {
	t.Helper()
	if s.Lease == nil {
		t.Fatal("missing lease")
	}
	next, _ := applyForTest(t, s, Release{
		Actor: s.Lease.Holder, LeaseID: s.Lease.ID,
		Generation: s.Generation, NeutralReceipt: "neutral-proof",
	}, at)
	return next
}

func TestPriorityFIFOAndSinglePendingGrant(t *testing.T) {
	s := boardForTest(t)
	var events []Event
	s, events = applyForTest(t, s, request("first-ai", ClassAI), testEpoch)
	if s.Phase != GrantPending || s.Lease.ID != "lease-first-ai" || len(events) != 2 {
		t.Fatalf("first request did not become sole pending grant: %#v, %#v", s, events)
	}
	s, _ = applyForTest(t, s, request("second-ai", ClassAI), testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, request("ci-one", ClassCI), testEpoch.Add(2*time.Second))
	s, _ = applyForTest(t, s, request("human-one", ClassHuman), testEpoch.Add(3*time.Second))
	s, _ = applyForTest(t, s, request("ci-two", ClassCI), testEpoch.Add(4*time.Second))
	s, _ = applyForTest(t, s, request("human-two", ClassHuman), testEpoch.Add(5*time.Second))
	if len(s.Queue) != 5 || s.Lease.ID != "lease-first-ai" {
		t.Fatalf("pending lease was displaced: %#v", s)
	}
	s = ack(t, s, testEpoch.Add(6*time.Second))
	if s.Phase != YieldRequested {
		t.Fatalf("higher-priority waiter did not request yield: %s", s.Phase)
	}
	s, _ = applyForTest(t, s, BeginDrain{Actor: "owner-first-ai", LeaseID: s.Lease.ID, Generation: s.Generation}, testEpoch.Add(7*time.Second))
	s = releaseNeutral(t, s, testEpoch.Add(8*time.Second))
	for i, want := range []string{"lease-human-one", "lease-human-two", "lease-ci-one", "lease-ci-two", "lease-second-ai"} {
		if s.Phase != GrantPending || s.Lease.ID != want {
			t.Fatalf("grant %d = %#v; want %s", i, s.Lease, want)
		}
		s = ack(t, s, testEpoch.Add(time.Duration(10+i*2)*time.Second))
		s = releaseNeutral(t, s, testEpoch.Add(time.Duration(11+i*2)*time.Second))
	}
	if s.Phase != Ready || s.Lease != nil || len(s.Queue) != 0 || s.Generation != 6 {
		t.Fatalf("final state: %#v", s)
	}
}

func TestGrantAckRequiresPersistedGeneration(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
	if err := CanStartSegment(s, Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Generation}, testEpoch, time.Second, time.Second); !IsCode(err, RecoveryNecessary) {
		t.Fatalf("pending grant usable: %v", err)
	}
	before := s
	next, events, err := Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: s.Lease.ID, Generation: s.Generation, InstalledGeneration: 0}, testEpoch.Add(time.Second))
	if !IsCode(err, StaleGeneration) || next.Phase != GrantPending || next.Generation != before.Generation || len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("uninstalled grant accepted or unaudited: %#v %#v %v", next, events, err)
	}
	s = ack(t, next, testEpoch.Add(2*time.Second))
	if err := CanStartSegment(s, Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Generation}, testEpoch.Add(3*time.Second), time.Second, time.Second); err != nil {
		t.Fatalf("installed grant denied: %v", err)
	}
}

func TestHumanYieldIsCooperative(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	oldToken := Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Generation}
	s, events := applyForTest(t, s, request("human", ClassHuman), testEpoch.Add(2*time.Second))
	if s.Phase != YieldRequested || len(events) != 2 || events[1].Kind != YieldAsked || s.Lease.ID != oldToken.LeaseID {
		t.Fatalf("human did not request cooperative yield: %#v %#v", s, events)
	}
	if err := CanStartSegment(s, oldToken, testEpoch.Add(3*time.Second), time.Second, time.Second); !IsCode(err, RecoveryNecessary) {
		t.Fatalf("new CI segment allowed after yield request: %v", err)
	}
	s, _ = applyForTest(t, s, BeginDrain{Actor: "owner-ci", LeaseID: oldToken.LeaseID, Generation: oldToken.Generation}, testEpoch.Add(3*time.Second))
	if s.Phase != Draining || s.Lease.ID != oldToken.LeaseID {
		t.Fatalf("drain stole the board: %#v", s)
	}
	s = releaseNeutral(t, s, testEpoch.Add(4*time.Second))
	if s.Phase != GrantPending || s.Lease.ID != "lease-human" {
		t.Fatalf("human not next: %#v", s)
	}
	if err := CanStartSegment(s, oldToken, testEpoch.Add(5*time.Second), time.Second, time.Second); !IsCode(err, RecoveryNecessary) {
		t.Fatalf("old token remained usable: %v", err)
	}
}

func TestExpiryAndUnverifiedReleaseRequireRecovery(t *testing.T) {
	for _, tc := range []struct {
		name   string
		finish func(Snapshot) (Snapshot, []Event, error)
	}{
		{"expiry", func(s Snapshot) (Snapshot, []Event, error) {
			return Apply(s, Tick{Actor: "server"}, testEpoch.Add(21*time.Minute))
		}},
		{"unverified-release", func(s Snapshot) (Snapshot, []Event, error) {
			return Apply(s, Release{Actor: "owner-ci", LeaseID: s.Lease.ID, Generation: s.Generation}, testEpoch.Add(3*time.Second))
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := boardForTest(t)
			s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
			s = ack(t, s, testEpoch.Add(time.Second))
			s, _ = applyForTest(t, s, request("human", ClassHuman), testEpoch.Add(2*time.Second))
			s, _, err := tc.finish(s)
			if err != nil || s.Phase != RecoveryRequired || s.Lease == nil || len(s.Queue) != 1 {
				t.Fatalf("unsafe handoff: %#v %v", s, err)
			}
			s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "profile-v1", Reason: "verify neutral"}, testEpoch.Add(22*time.Minute))
			s, _ = applyForTest(t, s, CompleteRecovery{Actor: "board-agent", NeutralReceipt: "verified-neutral", AgentHighWater: s.Generation}, testEpoch.Add(23*time.Minute))
			if s.Phase != GrantPending || s.Lease.ID != "lease-human" {
				t.Fatalf("queued human not granted after recovery: %#v", s)
			}
		})
	}
}

func TestRestoredDatabaseGenerationQuarantinesThenReconciles(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ai", ClassAI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, request("human", ClassHuman), testEpoch.Add(2*time.Second))
	s, events := applyForTest(t, s, ObserveAgentGeneration{Actor: "board-agent", HighWater: 8}, testEpoch.Add(3*time.Second))
	if s.Phase != Quarantined || s.AgentHighWater != 8 || events[0].Kind != BoardQuarantined {
		t.Fatalf("rollback not quarantined: %#v %#v", s, events)
	}
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "restore-v1", Reason: "physical clear"}, testEpoch.Add(4*time.Second))
	s, events = applyForTest(t, s, CompleteRecovery{Actor: "operator", NeutralReceipt: "physical-clear-verified", AgentHighWater: 8}, testEpoch.Add(5*time.Second))
	if s.Phase != GrantPending || s.Generation != 9 || s.AgentHighWater != 8 || s.Lease.ID != "lease-human" {
		t.Fatalf("reconciliation did not advance fence: %#v", s)
	}
	if len(events) != 3 || events[0].Kind != GenerationReconciled || events[1].Kind != RecoveryFinished || events[2].Kind != GrantCreated {
		t.Fatalf("missing reconciliation audit: %#v", events)
	}
}

func TestCurrentTokenAndExtensionBounds(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	valid := Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Generation}
	stale := valid
	stale.Generation--
	if err := CanStartSegment(s, stale, testEpoch.Add(2*time.Second), time.Second, time.Second); !IsCode(err, StaleGeneration) {
		t.Fatalf("stale token: %v", err)
	}
	if err := CanStartSegment(s, valid, testEpoch.Add(19*time.Minute), 2*time.Minute, time.Second); !IsCode(err, Deadline) {
		t.Fatalf("overlong segment: %v", err)
	}
	if err := CanStartSegment(s, valid, testEpoch.Add(20*time.Minute), time.Second, time.Second); !IsCode(err, Expired) {
		t.Fatalf("expired token: %v", err)
	}
	s, _ = applyForTest(t, s, request("human", ClassHuman), testEpoch.Add(2*time.Second))
	oldExpiry := s.Lease.ExpiresAt
	s, _ = applyForTest(t, s, Extend{Actor: "owner-ci", LeaseID: s.Lease.ID, Generation: s.Generation, NewExpiry: oldExpiry.Add(10 * time.Minute), Reason: "finish flash safely"}, testEpoch.Add(3*time.Second))
	if s.Lease.DeadlineVersion != 2 || s.Lease.ContendedExtensionUsed != 10*time.Minute {
		t.Fatalf("extension not recorded: %#v", s.Lease)
	}
	next, events, err := Apply(s, Extend{Actor: "owner-ci", LeaseID: s.Lease.ID, Generation: s.Generation, NewExpiry: s.Lease.ExpiresAt.Add(time.Second), Reason: "more"}, testEpoch.Add(4*time.Second))
	if !IsCode(err, Denied) || next.Lease.ExpiresAt != s.Lease.ExpiresAt || len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("contended extension ceiling bypassed: %#v %#v %v", next.Lease, events, err)
	}
}

func TestDeadlineFenceFailClosedAndMonotonic(t *testing.T) {
	now := time.Now()
	fence, err := SeedDeadline(7, 1, now.Add(30*time.Second), now, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got := fence.Until.Sub(now); got != 27*time.Second {
		t.Fatalf("budget = %s, want 27s", got)
	}
	if err := fence.CanStartSegment(7, now, 20*time.Second, 5*time.Second); err != nil {
		t.Fatalf("bounded local segment denied: %v", err)
	}
	if err := fence.CanStartSegment(7, now, 25*time.Second, 5*time.Second); !IsCode(err, Deadline) {
		t.Fatalf("overlong local segment accepted: %v", err)
	}
	if _, err := SeedDeadline(7, 1, now.Add(30*time.Second), now, 3*time.Second); !IsCode(err, InvalidArgument) {
		t.Fatalf("unbounded clock offset accepted: %v", err)
	}
	if _, err := SeedDeadline(7, 1, now.Add(2*time.Second), now, time.Second); !IsCode(err, Expired) {
		t.Fatalf("spent conservative deadline accepted: %v", err)
	}
	later, err := RefreshDeadline(fence, 7, 1, now.Add(time.Minute), now.Add(time.Second), time.Second, false)
	if err != nil || later.Until.After(fence.Until) {
		t.Fatalf("heartbeat lengthened deadline: %#v %v", later, err)
	}
	shorter, err := RefreshDeadline(fence, 7, 1, now.Add(15*time.Second), now.Add(time.Second), time.Second, false)
	if err != nil || !shorter.Until.Before(fence.Until) {
		t.Fatalf("heartbeat failed to shorten: %#v %v", shorter, err)
	}
	if _, err := RefreshDeadline(fence, 7, 1, now.Add(time.Minute), now, time.Second, true); !IsCode(err, Conflict) {
		t.Fatalf("same-version extension accepted: %v", err)
	}
	extended, err := RefreshDeadline(fence, 7, 2, now.Add(time.Minute), now, time.Second, true)
	if err != nil || !extended.Until.After(fence.Until) {
		t.Fatalf("audited extension did not lengthen: %#v %v", extended, err)
	}
	revived, err := RefreshDeadline(fence, 7, 2, now.Add(time.Minute), fence.Until, time.Second, true)
	if !IsCode(err, Expired) || !revived.Until.IsZero() {
		t.Fatalf("expired local fence was revived: %#v %v", revived, err)
	}
	if _, err := RefreshDeadline(extended, 6, 3, now.Add(time.Hour), now, time.Second, true); !IsCode(err, StaleGeneration) {
		t.Fatalf("old generation extended authority: %v", err)
	}
	invalid, err := RefreshDeadline(fence, 7, 1, now.Add(time.Minute), now, 3*time.Second, false)
	if !IsCode(err, InvalidArgument) || !invalid.Until.IsZero() {
		t.Fatalf("invalid clock retained authority: %#v %v", invalid, err)
	}
}

func TestAgentLossAndAckMismatchFailClosed(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
	s, _ = applyForTest(t, s, request("human", ClassHuman), testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "server", Reason: "heartbeat missed"}, testEpoch.Add(2*time.Second))
	if s.Phase != RecoveryRequired || s.Lease.ID != "lease-ci" || len(s.Queue) != 1 {
		t.Fatalf("agent loss granted queued human: %#v", s)
	}
	if err := CanStartSegment(s, Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Generation}, testEpoch.Add(3*time.Second), time.Second, time.Second); !IsCode(err, RecoveryNecessary) {
		t.Fatalf("agent loss did not fence activity: %v", err)
	}
	other := boardForTest(t)
	other, _ = applyForTest(t, other, request("ci", ClassCI), testEpoch)
	other, _ = applyForTest(t, other, AcknowledgeGrant{Actor: "board-agent", LeaseID: other.Lease.ID, Generation: other.Generation, InstalledGeneration: 4}, testEpoch.Add(time.Second))
	if other.Phase != Quarantined || other.AgentHighWater != 4 {
		t.Fatalf("agent ahead of DB was not quarantined: %#v", other)
	}
}

func TestQueueIdempotencyCancellationAndYieldAuthorization(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("human", ClassHuman), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	currentVersion := s.Version
	s, events := applyForTest(t, s, request("human", ClassHuman), testEpoch.Add(2*time.Second))
	if len(events) != 0 || s.Version != currentVersion {
		t.Fatalf("active retry not idempotent: %#v %#v", s, events)
	}
	s, _ = applyForTest(t, s, request("ai", ClassAI), testEpoch.Add(3*time.Second))
	queuedVersion := s.Version
	s, events = applyForTest(t, s, request("ai", ClassAI), testEpoch.Add(4*time.Second))
	if len(events) != 0 || s.Version != queuedVersion || len(s.Queue) != 1 {
		t.Fatalf("queued retry not idempotent: %#v %#v", s, events)
	}
	bad := request("ai", ClassCI)
	next, events, err := Apply(s, bad, testEpoch.Add(5*time.Second))
	if !IsCode(err, Conflict) || len(next.Queue) != 1 || len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("request ID collision not rejected: %#v %#v %v", next, events, err)
	}
	next, events, err = Apply(s, RequestYield{Actor: "owner-ai", WaiterID: "ai"}, testEpoch.Add(6*time.Second))
	if !IsCode(err, Denied) || next.Phase != Active || events[0].Kind != ActionDenied {
		t.Fatalf("lower-priority yield request preempted human: %#v %#v %v", next, events, err)
	}
	s, events = applyForTest(t, s, CancelWaiter{Actor: "owner-ai", WaiterID: "ai"}, testEpoch.Add(7*time.Second))
	if len(s.Queue) != 0 || len(events) != 1 || events[0].Kind != WaitCancelled {
		t.Fatalf("cancel did not withdraw waiter: %#v %#v", s, events)
	}
	_, events, err = Apply(s, CancelWaiter{Actor: "owner-ai", WaiterID: "ai"}, testEpoch.Add(8*time.Second))
	if !IsCode(err, Conflict) || events[0].Kind != ActionDenied {
		t.Fatalf("missing waiter cancel not audited: %#v %v", events, err)
	}
	// A restored active snapshot may contain a higher waiter before the
	// corresponding yield event is materialized. The explicit command repairs it.
	s.Queue = []Waiter{{ID: "human-two", LeaseID: "lease-human-two", Holder: "second", Class: ClassHuman, Reason: "urgent", Duration: time.Minute, Sequence: s.NextSequence + 1, QueuedAt: testEpoch.Add(9 * time.Second)}}
	s.NextSequence++
	s.Lease.Class = ClassCI
	s, events = applyForTest(t, s, RequestYield{Actor: "server", WaiterID: "human-two"}, testEpoch.Add(9*time.Second))
	if s.Phase != YieldRequested || len(events) != 1 || events[0].Kind != YieldAsked {
		t.Fatalf("explicit yield failed: %#v %#v", s, events)
	}
}

func TestSameClassHumanContentionAndAgentObservation(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("first-human", ClassHuman), testEpoch)
	s, events := applyForTest(t, s, ObserveAgentGeneration{Actor: "board-agent", HighWater: s.Generation}, testEpoch.Add(time.Second))
	if s.Phase != GrantPending || s.AgentHighWater != s.Generation || len(events) != 1 || events[0].Kind != AgentObserved {
		t.Fatalf("observation improperly activated grant: %#v %#v", s, events)
	}
	s = ack(t, s, testEpoch.Add(2*time.Second))
	s, _ = applyForTest(t, s, request("second-human", ClassHuman), testEpoch.Add(3*time.Second))
	if s.Phase != Active {
		t.Fatalf("second human preempted first: %s", s.Phase)
	}
	s, _ = applyForTest(t, s, Extend{Actor: "owner-first-human", LeaseID: s.Lease.ID, Generation: s.Generation, NewExpiry: s.Lease.ExpiresAt.Add(10 * time.Minute), Reason: "safe wrap-up"}, testEpoch.Add(4*time.Second))
	if s.Lease.ContendedExtensionUsed != 10*time.Minute {
		t.Fatalf("human contention limit not charged: %#v", s.Lease)
	}
	_, events, err := Apply(s, Extend{Actor: "owner-first-human", LeaseID: s.Lease.ID, Generation: s.Generation, NewExpiry: s.Lease.ExpiresAt.Add(time.Second), Reason: "more"}, testEpoch.Add(5*time.Second))
	if !IsCode(err, Denied) || events[0].Kind != ActionDenied {
		t.Fatalf("second contended extension accepted: %#v %v", events, err)
	}
}

func TestManualQuarantineRequiresNeutralRecovery(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, Quarantine{Actor: "operator", Reason: "fixture mismatch"}, testEpoch)
	s, _ = applyForTest(t, s, request("human", ClassHuman), testEpoch.Add(time.Second))
	if s.Phase != Quarantined || s.Lease != nil || len(s.Queue) != 1 {
		t.Fatalf("quarantine granted board: %#v", s)
	}
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "profile-v1", Reason: "inspect"}, testEpoch.Add(2*time.Second))
	_, events, err := Apply(s, CompleteRecovery{Actor: "operator"}, testEpoch.Add(3*time.Second))
	if !IsCode(err, InvalidArgument) || events[0].Kind != ActionDenied {
		t.Fatalf("receipt-free recovery accepted: %#v %v", events, err)
	}
	s, _ = applyForTest(t, s, CompleteRecovery{Actor: "operator", NeutralReceipt: "proof", AgentHighWater: 0}, testEpoch.Add(4*time.Second))
	if s.Phase != GrantPending || s.Lease.ID != "lease-human" {
		t.Fatalf("verified recovery did not grant waiter: %#v", s)
	}
}

func TestCancelledHigherWaiterClearsOnlyUnstartedYield(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, request("human-one", ClassHuman), testEpoch.Add(2*time.Second))
	s, _ = applyForTest(t, s, request("human-two", ClassHuman), testEpoch.Add(3*time.Second))
	s, events := applyForTest(t, s, CancelWaiter{Actor: "owner-human-one", WaiterID: "human-one"}, testEpoch.Add(4*time.Second))
	if s.Phase != YieldRequested || len(events) != 1 || events[0].Kind != WaitCancelled {
		t.Fatalf("yield cleared with another human still waiting: %#v %#v", s, events)
	}
	s, events = applyForTest(t, s, CancelWaiter{Actor: "owner-human-two", WaiterID: "human-two"}, testEpoch.Add(5*time.Second))
	if s.Phase != Active || !s.Lease.YieldRequestedAt.IsZero() || len(events) != 2 || events[1].Kind != YieldCleared {
		t.Fatalf("last human withdrew but yield remained latched: %#v %#v", s, events)
	}
	s, _ = applyForTest(t, s, request("human-three", ClassHuman), testEpoch.Add(6*time.Second))
	s, _ = applyForTest(t, s, BeginDrain{Actor: "owner-ci", LeaseID: s.Lease.ID, Generation: s.Generation}, testEpoch.Add(7*time.Second))
	s, events = applyForTest(t, s, CancelWaiter{Actor: "owner-human-three", WaiterID: "human-three"}, testEpoch.Add(8*time.Second))
	if s.Phase != Draining || len(events) != 1 || events[0].Kind != WaitCancelled {
		t.Fatalf("cancel incorrectly reversed in-progress drain: %#v %#v", s, events)
	}
}

func TestAgentLossDuringRecoveryQuarantines(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "server", Reason: "heartbeat lost"}, testEpoch.Add(2*time.Second))
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "profile-v1", Reason: "restore"}, testEpoch.Add(3*time.Second))
	s, events := applyForTest(t, s, AgentUnavailable{Actor: "server", Reason: "heartbeat lost again"}, testEpoch.Add(4*time.Second))
	if s.Phase != Quarantined || len(events) != 1 || events[0].Kind != BoardQuarantined {
		t.Fatalf("agent loss during recovery was silent: %#v %#v", s, events)
	}
	_, events, err := Apply(s, CompleteRecovery{Actor: "operator", NeutralReceipt: "old-proof", AgentHighWater: s.AgentHighWater}, testEpoch.Add(5*time.Second))
	if !IsCode(err, Conflict) || len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("stale recovery completed after agent loss: %#v %v", events, err)
	}
}

func TestValidateRejectsMalformedPersistedWaiters(t *testing.T) {
	base := boardForTest(t)
	base.NextSequence = 2
	base.Queue = []Waiter{{ID: "one", LeaseID: "lease-one", Holder: "owner", Class: ClassCI, Reason: "reason", Duration: time.Minute, Sequence: 1, QueuedAt: testEpoch}}
	if err := Validate(base); err != nil {
		t.Fatalf("valid baseline: %v", err)
	}
	for _, tc := range []struct {
		name string
		edit func(*Snapshot)
	}{
		{"blank-holder", func(s *Snapshot) { s.Queue[0].Holder = "" }},
		{"blank-reason", func(s *Snapshot) { s.Queue[0].Reason = "" }},
		{"excess-duration", func(s *Snapshot) { s.Queue[0].Duration = 3 * time.Hour }},
		{"zero-queued-time", func(s *Snapshot) { s.Queue[0].QueuedAt = time.Time{} }},
		{"duplicate-sequence", func(s *Snapshot) {
			s.Queue = append(s.Queue, Waiter{ID: "two", LeaseID: "lease-two", Holder: "owner-two", Class: ClassAI, Reason: "reason", Duration: time.Minute, Sequence: 1, QueuedAt: testEpoch})
		}},
		{"queue-over-limit", func(s *Snapshot) { s.Queue = make([]Waiter, MaxWaiters+1) }},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := clone(base)
			tc.edit(&s)
			if err := Validate(s); !IsCode(err, Conflict) {
				t.Fatalf("invalid persisted waiter accepted: %v", err)
			}
		})
	}
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("current", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s.NextSequence = 1
	s.Queue = []Waiter{{ID: s.Lease.WaiterID, LeaseID: "different", Holder: "other", Class: ClassHuman, Reason: "reason", Duration: time.Minute, Sequence: 1, QueuedAt: testEpoch}}
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("active waiter ID collision accepted: %v", err)
	}
	s.Queue[0].ID = "different"
	s.Queue[0].LeaseID = s.Lease.ID
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("active lease ID collision accepted: %v", err)
	}
}

func TestMaxWaitersRejectsNewClaim(t *testing.T) {
	s := boardForTest(t)
	s.NextSequence = MaxWaiters
	for i := 1; i <= MaxWaiters; i++ {
		s.Queue = append(s.Queue, Waiter{ID: fmtID(i), LeaseID: "lease-" + fmtID(i), Holder: "owner", Class: ClassAI, Reason: "queued", Duration: time.Minute, Sequence: uint64(i), QueuedAt: testEpoch})
	}
	if err := Validate(s); err != nil {
		t.Fatal(err)
	}
	next, events, err := Apply(s, request("extra-human", ClassHuman), testEpoch.Add(time.Second))
	if !IsCode(err, Denied) || len(next.Queue) != MaxWaiters || len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("queue cap bypassed: %d %#v %v", len(next.Queue), events, err)
	}
}

func TestHighRiskDecisionMCDCVectors(t *testing.T) {
	// MC/DC: every input of the compound enqueue validation independently
	// changes the base valid decision to invalid while all others stay valid.
	base := request("vector", ClassCI)
	for _, tc := range []struct {
		name string
		edit func(*Enqueue)
	}{
		{"control", func(*Enqueue) {}},
		{"actor", func(c *Enqueue) { c.Actor = "" }},
		{"waiter-id", func(c *Enqueue) { c.Waiter.ID = "" }},
		{"lease-id", func(c *Enqueue) { c.Waiter.LeaseID = "" }},
		{"holder", func(c *Enqueue) { c.Waiter.Holder = "" }},
		{"reason", func(c *Enqueue) { c.Waiter.Reason = "" }},
		{"class", func(c *Enqueue) { c.Waiter.Class = 0 }},
		{"duration-zero", func(c *Enqueue) { c.Waiter.Duration = 0 }},
		{"duration-ceiling", func(c *Enqueue) { c.Waiter.Duration = 3 * time.Hour }},
	} {
		t.Run("enqueue-"+tc.name, func(t *testing.T) {
			c := base
			tc.edit(&c)
			s := boardForTest(t)
			_, _, err := Apply(s, c, testEpoch)
			if tc.name == "control" && err != nil {
				t.Fatalf("control rejected: %v", err)
			}
			if tc.name != "control" && !IsCode(err, InvalidArgument) {
				t.Fatalf("condition did not independently reject: %v", err)
			}
		})
	}
	// MC/DC: each token identity field independently changes a valid segment
	// decision to stale, while the phase, time, and duration remain fixed.
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	token := Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Generation}
	for _, tc := range []struct {
		name string
		edit func(*Token)
	}{
		{"control", func(*Token) {}},
		{"board", func(token *Token) { token.BoardID = "wrong" }},
		{"lease", func(token *Token) { token.LeaseID = "wrong" }},
		{"generation", func(token *Token) { token.Generation++ }},
	} {
		t.Run("token-"+tc.name, func(t *testing.T) {
			candidate := token
			tc.edit(&candidate)
			err := CanStartSegment(s, candidate, testEpoch.Add(2*time.Second), time.Second, time.Second)
			if tc.name == "control" && err != nil {
				t.Fatalf("control rejected: %v", err)
			}
			if tc.name != "control" && !IsCode(err, StaleGeneration) {
				t.Fatalf("condition did not independently reject: %v", err)
			}
		})
	}
	// MC/DC: each clock validation input independently invalidates an
	// otherwise valid conservative deadline calculation.
	now := time.Now()
	for _, tc := range []struct {
		name   string
		expiry time.Time
		local  time.Time
		offset time.Duration
	}{
		{"control", now.Add(time.Minute), now, time.Second},
		{"expiry-zero", time.Time{}, now, time.Second},
		{"local-zero", now.Add(time.Minute), time.Time{}, time.Second},
		{"offset-negative", now.Add(time.Minute), now, -time.Second},
		{"offset-unbounded", now.Add(time.Minute), now, 3 * time.Second},
	} {
		t.Run("clock-"+tc.name, func(t *testing.T) {
			_, err := SeedDeadline(1, 1, tc.expiry, tc.local, tc.offset)
			if tc.name == "control" && err != nil {
				t.Fatalf("control rejected: %v", err)
			}
			if tc.name != "control" && !IsCode(err, InvalidArgument) {
				t.Fatalf("condition did not independently reject: %v", err)
			}
		})
	}
}

func TestFailedGrantRollsBackQueueMutation(t *testing.T) {
	s := boardForTest(t)
	s.Generation = math.MaxUint64
	s.AgentHighWater = math.MaxUint64
	next, events, err := Apply(s, request("ci", ClassCI), testEpoch)
	if !IsCode(err, Conflict) || next.Phase != Ready || len(next.Queue) != 0 || len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("failed grant left phantom waiter: %#v %#v %v", next, events, err)
	}
}

func TestConcurrentRequestsNeverCreateTwoLeases(t *testing.T) {
	const contenders = 96
	s := boardForTest(t)
	var mu sync.Mutex
	var wg sync.WaitGroup
	errors := make(chan error, contenders)
	for i := 0; i < contenders; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			mu.Lock()
			defer mu.Unlock()
			c := request(fmtID(i), Class(i%3+1))
			next, _, err := Apply(s, c, testEpoch.Add(time.Duration(i)*time.Millisecond))
			if err != nil {
				errors <- err
				return
			}
			if err := Validate(next); err != nil {
				errors <- err
				return
			}
			s = next
		}(i)
	}
	wg.Wait()
	close(errors)
	for err := range errors {
		t.Error(err)
	}
	if s.Lease == nil || s.Phase != GrantPending || len(s.Queue) != contenders-1 || s.Generation != 1 {
		t.Fatalf("concurrent grant uniqueness lost: %#v", s)
	}
}

func fmtID(i int) string { return "request-" + strconv.Itoa(i) }
