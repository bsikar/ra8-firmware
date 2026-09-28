package board

import (
	"testing"
	"time"
)

// Once a board is in trouble, the commands that keep arriving at it are the
// ones the reducer answers least visibly. A holder still drains, a server
// still ticks, an agent still reports itself gone, an operator still starts a
// plan. The recovery phases are reached exactly when nobody is watching
// closely, so what matters is that each command is refused for the reason it
// was actually refused for, that the ones written to be idempotent really are,
// and that a board never leaves a recovery phase except by the one door that
// ends with a neutral receipt.
//
// The coverage pass puts beginDrain at 66.7%, beginRecovery and quarantine at
// 71.4% and agentUnavailable at 77.8%, and every missing branch is a refusal.

func troubledAt(offset time.Duration) time.Time {
	return testEpoch.Add(offset)
}

// troubledHeldBoard returns a board with an acknowledged, active lease held by
// owner-ci, plus the token fields a holder command needs.
func troubledHeldBoard(t *testing.T) (Snapshot, string, uint64) {
	t.Helper()
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci", ClassCI), troubledAt(time.Second))
	s = ack(t, s, troubledAt(2*time.Second))
	if s.Phase != Active || s.Lease == nil {
		t.Fatalf("board not active after ack: %s %#v", s.Phase, s.Lease)
	}
	return s, s.Lease.ID, s.Generation
}

// A drain is a claim that the holder stopped at a safe checkpoint, and it is
// only meaningful once somebody asked it to. An unasked drain is refused as
// Conflict, and refused without moving the board: a holder that reports a
// checkpoint nobody wanted is not evidence of anything.
func TestADrainNobodyAskedForIsRefused(t *testing.T) {
	s, leaseID, generation := troubledHeldBoard(t)
	next, events, err := Apply(s, BeginDrain{Actor: "owner-ci", LeaseID: leaseID, Generation: generation}, troubledAt(3*time.Second))
	if !IsCode(err, Conflict) {
		t.Fatalf("unasked drain: %v, want Conflict", err)
	}
	if next.Phase != Active {
		t.Fatalf("refused drain moved the board to %s", next.Phase)
	}
	if len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("refused drain filed %#v, want one ActionDenied", events)
	}
}

// A drain with no holder identity on it is the caller's mistake rather than a
// verdict about the board, and it is caught before the phase is consulted: the
// same command against a board that genuinely did request a yield still reads
// InvalidArgument.
func TestADrainWithNoHolderIsTheCallersMistake(t *testing.T) {
	s, leaseID, generation := troubledHeldBoard(t)
	if _, _, err := Apply(s, BeginDrain{LeaseID: leaseID, Generation: generation}, troubledAt(3*time.Second)); !IsCode(err, InvalidArgument) {
		t.Fatalf("anonymous drain on an active board: %v, want InvalidArgument", err)
	}
	s, _ = applyForTest(t, s, request("human", ClassHuman), troubledAt(3*time.Second))
	if s.Phase != YieldRequested {
		t.Fatalf("board did not enter YieldRequested, got %s", s.Phase)
	}
	if _, _, err := Apply(s, BeginDrain{LeaseID: leaseID, Generation: generation}, troubledAt(4*time.Second)); !IsCode(err, InvalidArgument) {
		t.Fatalf("anonymous drain on a yielding board: %v, want InvalidArgument", err)
	}
}

// A drain naming a lease or generation that is not the current one is stale,
// not a conflict. The distinction is the whole point of current(): a command
// from a previous holder must never read as a disagreement about the phase.
func TestADrainFromAPreviousLeaseIsStale(t *testing.T) {
	s, leaseID, generation := troubledHeldBoard(t)
	s, _ = applyForTest(t, s, request("human", ClassHuman), troubledAt(3*time.Second))
	if _, _, err := Apply(s, BeginDrain{Actor: "owner-ci", LeaseID: "lease-someone-else", Generation: generation}, troubledAt(4*time.Second)); !IsCode(err, StaleGeneration) {
		t.Fatalf("drain naming another lease: %v, want StaleGeneration", err)
	}
	if _, _, err := Apply(s, BeginDrain{Actor: "owner-ci", LeaseID: leaseID, Generation: generation + 1}, troubledAt(4*time.Second)); !IsCode(err, StaleGeneration) {
		t.Fatalf("drain naming a later generation: %v, want StaleGeneration", err)
	}
}

// The drain that was asked for is admitted, and it does not release the
// board: the lease is still there, still the same one, and the holder is still
// on the hook for a receipt.
func TestTheDrainThatWasAskedForKeepsTheLease(t *testing.T) {
	s, leaseID, generation := troubledHeldBoard(t)
	s, _ = applyForTest(t, s, request("human", ClassHuman), troubledAt(3*time.Second))
	next, events := applyForTest(t, s, BeginDrain{Actor: "owner-ci", LeaseID: leaseID, Generation: generation}, troubledAt(4*time.Second))
	if next.Phase != Draining {
		t.Fatalf("phase after drain = %s, want Draining", next.Phase)
	}
	if next.Lease == nil || next.Lease.ID != leaseID {
		t.Fatalf("drain moved the lease: %#v", next.Lease)
	}
	if len(events) != 1 || events[0].Kind != DrainStarted {
		t.Fatalf("drain filed %#v, want one DrainStarted", events)
	}
	// A second drain is no longer the asked-for one.
	if _, _, err := Apply(next, BeginDrain{Actor: "owner-ci", LeaseID: leaseID, Generation: generation}, troubledAt(5*time.Second)); !IsCode(err, Conflict) {
		t.Fatalf("second drain: %v, want Conflict", err)
	}
}

// An agent reporting itself gone says the same thing however many times it
// says it. Once the board is already awaiting recovery, the report changes
// nothing and files nothing, so a flapping agent cannot bury the event that
// recorded why recovery was needed in the first place.
func TestARepeatedUnavailableReportChangesNothing(t *testing.T) {
	s, _, _ := troubledHeldBoard(t)
	s, events := applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "heartbeat lost"}, troubledAt(3*time.Second))
	if s.Phase != RecoveryRequired {
		t.Fatalf("phase = %s, want RecoveryRequired", s.Phase)
	}
	if len(events) != 1 || events[0].Kind != RecoveryNeeded || events[0].Reason != "heartbeat lost" {
		t.Fatalf("first report filed %#v", events)
	}
	before := s
	s, again := applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "heartbeat lost again"}, troubledAt(4*time.Second))
	if len(again) != 0 {
		t.Fatalf("repeated report filed %#v, want nothing", again)
	}
	if s.Phase != before.Phase || s.Version != before.Version {
		t.Fatalf("repeated report moved the board: %s v%d, was %s v%d", s.Phase, s.Version, before.Phase, before.Version)
	}
}

// The same report during a recovery is not idempotent, and must not be: the
// agent going away while somebody is actively restoring the fixture leaves the
// board in a state nobody can vouch for, so it escalates to quarantine and the
// reason it escalated is carried into the event.
func TestAnAgentLostDuringRecoveryQuarantinesTheBoard(t *testing.T) {
	s, _, _ := troubledHeldBoard(t)
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "heartbeat lost"}, troubledAt(3*time.Second))
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "plan-7", Reason: "reflash fixture"}, troubledAt(4*time.Second))
	if s.Phase != Recovering {
		t.Fatalf("phase = %s, want Recovering", s.Phase)
	}
	s, events := applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "power cycled"}, troubledAt(5*time.Second))
	if s.Phase != Quarantined {
		t.Fatalf("phase = %s, want Quarantined", s.Phase)
	}
	if len(events) != 1 || events[0].Kind != BoardQuarantined {
		t.Fatalf("escalation filed %#v, want one BoardQuarantined", events)
	}
	if events[0].Reason != "agent unavailable during recovery: power cycled" {
		t.Fatalf("escalation reason = %q, which loses why it escalated", events[0].Reason)
	}
	// Already quarantined, so a further report is silent again.
	_, again := applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "still gone"}, troubledAt(6*time.Second))
	if len(again) != 0 {
		t.Fatalf("report against a quarantined board filed %#v, want nothing", again)
	}
}

// A report with no actor or no reason is refused outright. The reason is the
// only record of why a board stopped taking work, so an empty one is not a
// report at all.
func TestAnUnavailableReportNeedsAnActorAndAReason(t *testing.T) {
	s, _, _ := troubledHeldBoard(t)
	for _, c := range []AgentUnavailable{
		{Reason: "heartbeat lost"},
		{Actor: "agent"},
		{},
	} {
		next, _, err := Apply(s, c, troubledAt(3*time.Second))
		if !IsCode(err, InvalidArgument) {
			t.Fatalf("%#v: %v, want InvalidArgument", c, err)
		}
		if next.Phase != Active {
			t.Fatalf("%#v moved the board to %s", c, next.Phase)
		}
	}
}

// Recovery starts only from a board that is awaiting it. Starting one against
// a healthy board is a Conflict, and it is refused from every live phase, so a
// plan can never be run underneath a holder that still has authority.
func TestRecoveryCannotStartUnderneathALiveHolder(t *testing.T) {
	s, leaseID, generation := troubledHeldBoard(t)
	plan := BeginRecovery{Actor: "operator", PlanID: "plan-7", Reason: "reflash fixture"}
	if _, _, err := Apply(s, plan, troubledAt(3*time.Second)); !IsCode(err, Conflict) {
		t.Fatalf("recovery against an active board: %v, want Conflict", err)
	}
	yielding, _ := applyForTest(t, s, request("human", ClassHuman), troubledAt(3*time.Second))
	if _, _, err := Apply(yielding, plan, troubledAt(4*time.Second)); !IsCode(err, Conflict) {
		t.Fatalf("recovery against a yielding board: %v, want Conflict", err)
	}
	draining, _ := applyForTest(t, yielding, BeginDrain{Actor: "owner-ci", LeaseID: leaseID, Generation: generation}, troubledAt(4*time.Second))
	if _, _, err := Apply(draining, plan, troubledAt(5*time.Second)); !IsCode(err, Conflict) {
		t.Fatalf("recovery against a draining board: %v, want Conflict", err)
	}
	free := boardForTest(t)
	if _, _, err := Apply(free, plan, troubledAt(time.Second)); !IsCode(err, Conflict) {
		t.Fatalf("recovery against a free board: %v, want Conflict", err)
	}
}

// A plan needs all three of actor, plan and reason, and the check runs ahead
// of the phase check: a nameless plan against a board that is genuinely
// awaiting recovery still reads as the caller's mistake, so an operator is
// never told the board refused a plan it never actually saw.
func TestARecoveryPlanNeedsAllThreeOfItsFields(t *testing.T) {
	s, _, _ := troubledHeldBoard(t)
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "heartbeat lost"}, troubledAt(3*time.Second))
	for _, c := range []BeginRecovery{
		{PlanID: "plan-7", Reason: "reflash"},
		{Actor: "operator", Reason: "reflash"},
		{Actor: "operator", PlanID: "plan-7"},
	} {
		next, _, err := Apply(s, c, troubledAt(4*time.Second))
		if !IsCode(err, InvalidArgument) {
			t.Fatalf("%#v: %v, want InvalidArgument", c, err)
		}
		if next.Phase != RecoveryRequired {
			t.Fatalf("%#v moved the board to %s", c, next.Phase)
		}
	}
}

// Recovery also starts from quarantine, which is the path out of the worst
// state the board has, and the plan and reason are both carried into the
// event so the history says which plan was run.
func TestRecoveryStartsFromQuarantineToo(t *testing.T) {
	s, _, _ := troubledHeldBoard(t)
	s, _ = applyForTest(t, s, Quarantine{Actor: "operator", Reason: "fixture smells burnt"}, troubledAt(3*time.Second))
	if s.Phase != Quarantined {
		t.Fatalf("phase = %s, want Quarantined", s.Phase)
	}
	s, events := applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "plan-9", Reason: "replace fixture"}, troubledAt(4*time.Second))
	if s.Phase != Recovering {
		t.Fatalf("phase = %s, want Recovering", s.Phase)
	}
	if len(events) != 1 || events[0].Kind != RecoveryStarted || events[0].Reason != "plan-9: replace fixture" {
		t.Fatalf("recovery filed %#v", events)
	}
}

// Quarantining a quarantined board is silent. An operator hitting the button
// twice, or a second detector firing on the same fault, must not file a second
// reason over the first one, because the first is the one that explains the
// board.
func TestQuarantineIsSilentOnceTheBoardIsQuarantined(t *testing.T) {
	s, _, _ := troubledHeldBoard(t)
	s, first := applyForTest(t, s, Quarantine{Actor: "operator", Reason: "fixture smells burnt"}, troubledAt(3*time.Second))
	if len(first) != 1 || first[0].Kind != BoardQuarantined || first[0].Reason != "fixture smells burnt" {
		t.Fatalf("first quarantine filed %#v", first)
	}
	before := s
	s, again := applyForTest(t, s, Quarantine{Actor: "someone else", Reason: "a different theory"}, troubledAt(4*time.Second))
	if len(again) != 0 {
		t.Fatalf("second quarantine filed %#v, want nothing", again)
	}
	if s.Version != before.Version || s.Phase != Quarantined {
		t.Fatalf("second quarantine moved the board: %s v%d, was %s v%d", s.Phase, s.Version, before.Phase, before.Version)
	}
}

// Quarantine needs an actor and a reason for the same argument the
// unavailable report does: the reason is the whole record.
func TestQuarantineNeedsAnActorAndAReason(t *testing.T) {
	s, _, _ := troubledHeldBoard(t)
	for _, c := range []Quarantine{{Reason: "burnt"}, {Actor: "operator"}, {}} {
		next, _, err := Apply(s, c, troubledAt(3*time.Second))
		if !IsCode(err, InvalidArgument) {
			t.Fatalf("%#v: %v, want InvalidArgument", c, err)
		}
		if next.Phase != Active {
			t.Fatalf("%#v moved the board to %s", c, next.Phase)
		}
	}
}

// The retained lease is evidence, and the recovery phases keep it. A board
// that went to recovery under a holder still names that holder, so the history
// says whose work was interrupted, right up until the recovery that clears it.
func TestTheRecoveryPhasesKeepTheInterruptedLeaseAsEvidence(t *testing.T) {
	s, leaseID, _ := troubledHeldBoard(t)
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "heartbeat lost"}, troubledAt(3*time.Second))
	if s.Lease == nil || s.Lease.ID != leaseID {
		t.Fatalf("RecoveryRequired dropped the lease: %#v", s.Lease)
	}
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "plan-7", Reason: "reflash"}, troubledAt(4*time.Second))
	if s.Lease == nil || s.Lease.ID != leaseID {
		t.Fatalf("Recovering dropped the lease: %#v", s.Lease)
	}
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "gone again"}, troubledAt(5*time.Second))
	if s.Phase != Quarantined || s.Lease == nil || s.Lease.ID != leaseID {
		t.Fatalf("Quarantined dropped the lease: %s %#v", s.Phase, s.Lease)
	}
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "plan-8", Reason: "again"}, troubledAt(6*time.Second))
	s, _ = applyForTest(t, s, CompleteRecovery{Actor: "operator", NeutralReceipt: "receipt-1", AgentHighWater: s.Generation}, troubledAt(7*time.Second))
	if s.Phase != Ready || s.Lease != nil {
		t.Fatalf("completed recovery left %s %#v, want a free Ready board", s.Phase, s.Lease)
	}
}

// The one door out. Every command that is not CompleteRecovery leaves a
// recovering board recovering, including the holder commands that would end a
// lease on a live board, so no interrupted holder can release its way out of a
// recovery somebody else started.
func TestOnlyACompletedRecoveryLeavesARecoveringBoard(t *testing.T) {
	s, leaseID, generation := troubledHeldBoard(t)
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "agent", Reason: "heartbeat lost"}, troubledAt(3*time.Second))
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "plan-7", Reason: "reflash"}, troubledAt(4*time.Second))
	holderCommands := []Command{
		Release{Actor: "owner-ci", LeaseID: leaseID, Generation: generation, NeutralReceipt: "receipt-x"},
		BeginDrain{Actor: "owner-ci", LeaseID: leaseID, Generation: generation},
		Extend{Actor: "owner-ci", LeaseID: leaseID, Generation: generation, NewExpiry: troubledAt(time.Hour), Reason: "more time"},
	}
	for _, c := range holderCommands {
		next, _, err := Apply(s, c, troubledAt(5*time.Second))
		if err == nil {
			t.Fatalf("%T was admitted against a recovering board", c)
		}
		if next.Phase != Recovering {
			t.Fatalf("%T moved a recovering board to %s", c, next.Phase)
		}
		if next.Lease == nil || next.Lease.ID != leaseID {
			t.Fatalf("%T disturbed the retained lease: %#v", c, next.Lease)
		}
	}
	done, events := applyForTest(t, s, CompleteRecovery{Actor: "operator", NeutralReceipt: "receipt-1", AgentHighWater: s.Generation}, troubledAt(6*time.Second))
	if done.Phase != Ready || done.Lease != nil {
		t.Fatalf("recovery left %s %#v", done.Phase, done.Lease)
	}
	if len(events) == 0 || events[0].Kind != RecoveryFinished {
		t.Fatalf("completed recovery filed %#v", events)
	}
}
