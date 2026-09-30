// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package board

import (
	"math"
	"testing"
	"time"
)

// The refusals this reducer files before it transitions anything, and the two
// it files on behalf of a board whose own numbers no longer agree. Every one
// of them is a door an operator or an agent arrives at with a request that
// looks ordinary, so what matters is that the answer names the reason rather
// than the last thing to fail.

func refusalAt(offset time.Duration) time.Time { return testEpoch.Add(offset) }

// refusalHeld is an Active board held by agent-a under lease l-1, with the
// agent's installed generation caught up to the server's.
func refusalHeld(t *testing.T) Snapshot {
	t.Helper()
	s, err := New("board-refusals")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	s, _ = applyForTest(t, s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-1", LeaseID: "l-1", Holder: "agent-a", Class: ClassAI,
		Reason: "experiment", Duration: time.Hour,
	}}, refusalAt(0))
	if s.Phase != GrantPending {
		t.Fatalf("board is %v, not pending a grant", s.Phase)
	}
	s, _ = applyForTest(t, s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1",
		Generation: s.Generation, InstalledGeneration: s.Generation}, refusalAt(0))
	if s.Phase != Active || s.Lease == nil {
		t.Fatalf("board is %v with lease %v, not active", s.Phase, s.Lease)
	}
	return s
}

// refusalFiled reads the structural error out of a refusal and checks its code.
func refusalFiled(t *testing.T, err error, code Code, detail string) {
	t.Helper()
	if err == nil {
		t.Fatalf("a %s refusal was not filed", detail)
	}
	var structural *Error
	if !asStructuralError(err, &structural) {
		t.Fatalf("%s answered %T, not a board error: %v", detail, err, err)
	}
	if structural.Code != code {
		t.Fatalf("%s answered %v, not %v: %v", detail, structural.Code, code, err)
	}
}

// A snapshot's version is the store's optimistic-concurrency token, and it is
// the one number this reducer cannot carry past its ceiling. Wrapping it would
// let a stale writer's compare-and-set succeed against a newer row, so the
// board refuses the transition and leaves the snapshot exactly as it was: the
// operator's next move is a migration, not a retry.
func TestAVersionAtItsCeilingIsRefusedRatherThanWrapped(t *testing.T) {
	s := boardForTest(t)
	s.Version = math.MaxUint64
	if err := Validate(s); err != nil {
		t.Fatalf("an exhausted version is not itself an invalid board: %v", err)
	}

	after, events, err := Apply(s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-1", LeaseID: "l-1", Holder: "agent-a", Class: ClassAI,
		Reason: "experiment", Duration: time.Hour,
	}}, refusalAt(0))
	refusalFiled(t, err, Conflict, "a request against an exhausted version")

	// The refusal hands back the board untouched rather than a half-applied
	// one, and files no events for a transition that did not happen.
	if after.Version != math.MaxUint64 || len(after.Queue) != 0 || after.Phase != Ready {
		t.Fatalf("the refused board moved: version %d, %d queued, phase %v",
			after.Version, len(after.Queue), after.Phase)
	}
	if len(events) != 0 {
		t.Fatalf("a refused transition filed %d events", len(events))
	}

	// One below the ceiling is an ordinary request, so the refusal is the
	// ceiling itself and not the neighbourhood of it.
	s.Version = math.MaxUint64 - 1
	if _, _, err := Apply(s, Tick{Actor: "server"}, refusalAt(0)); err != nil {
		t.Fatalf("a board one version below the ceiling was refused: %v", err)
	}
}

// strangeCommand satisfies Command without being a command this reducer
// knows. Nothing in the tree can construct one, which is the point: the
// interface is unexported, so a command from outside this package cannot
// exist and the default arm exists for a case added here and wired up
// nowhere.
type strangeCommand struct{ Actor string }

func (strangeCommand) boardCommand() {}

// A command with no arm is refused, and the refusal names no actor. Reading
// an Actor field off an unknown command by reflection would put an
// unauthenticated string into the audit record, so the reducer declines to
// guess: the identity on a denial event is one this switch recognised.
func TestACommandWithNoArmIsRefusedWithoutNamingAnActor(t *testing.T) {
	s := refusalHeld(t)

	if actor := commandActor(strangeCommand{Actor: "smuggled"}); actor != "" {
		t.Fatalf("an unknown command named %q as its actor", actor)
	}

	after, events, err := Apply(s, strangeCommand{Actor: "smuggled"}, refusalAt(time.Minute))
	refusalFiled(t, err, InvalidArgument, "an unknown command")
	if after.Phase != Active || after.Lease == nil || after.Lease.ID != s.Lease.ID {
		t.Fatalf("an unknown command moved the board to %v with lease %v", after.Phase, after.Lease)
	}
	// The version still moves, because the denial itself is committed: the
	// caller writes the snapshot and its events in one transaction, so a
	// recorded refusal has to advance the row it is recorded against.
	if after.Version != s.Version+1 {
		t.Fatalf("a recorded denial left the version at %d, from %d", after.Version, s.Version)
	}
	// The denial is still recorded, because a rejected request is evidence.
	// What it must not carry is the actor the command asserted about itself.
	for _, e := range events {
		if e.Actor == "smuggled" {
			t.Fatalf("a denial event carried the unknown command's own actor: %+v", e)
		}
	}
}

// The segment gate is asked before every attempt, so its arguments are
// checked before the board is read at all: a caller that passes no bound has
// a bug in the caller, and answering it with anything about the board's phase
// would send whoever reads the log looking at the wrong machine.
func TestASegmentIsRefusedOnItsOwnArgumentsBeforeTheBoardIsRead(t *testing.T) {
	s := refusalHeld(t)
	token := Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Lease.Generation}
	sound := refusalAt(time.Minute)

	for _, bad := range []struct {
		name   string
		now    time.Time
		bound  time.Duration
		margin time.Duration
	}{
		{name: "no bound", now: sound, bound: 0, margin: time.Second},
		{name: "a negative bound", now: sound, bound: -time.Second, margin: time.Second},
		{name: "a negative margin", now: sound, bound: time.Minute, margin: -time.Second},
		{name: "no time", now: time.Time{}, bound: time.Minute, margin: time.Second},
	} {
		err := CanStartSegment(s, token, bad.now, bad.bound, bad.margin)
		refusalFiled(t, err, InvalidArgument, bad.name)
	}

	// A margin of zero is a caller that declares no recovery reserve, which
	// is a choice rather than a mistake.
	if err := CanStartSegment(s, token, sound, time.Minute, 0); err != nil {
		t.Fatalf("a zero recovery margin was refused: %v", err)
	}
}

// An agent that has not durably installed the generation it was granted is
// not allowed to start work under it. The board is active and the token
// matches, so the refusal has to come from the high-water mark, and it has to
// say recovery rather than staleness: the grant is current, the agent's
// record of it is not.
func TestASegmentIsRefusedWhileTheAgentHasNotInstalledTheGrant(t *testing.T) {
	s := refusalHeld(t)
	token := Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Lease.Generation}
	sound := refusalAt(time.Minute)

	if err := CanStartSegment(s, token, sound, time.Minute, time.Second); err != nil {
		t.Fatalf("a caught-up agent was refused a segment: %v", err)
	}

	behind := clone(s)
	behind.AgentHighWater = 0
	err := CanStartSegment(behind, token, sound, time.Minute, time.Second)
	refusalFiled(t, err, RecoveryNecessary, "an agent that has not installed the grant")

	// Not reported as a stale token: the token is the current one, which is
	// what separates this from an agent holding an old grant.
	var structural *Error
	if asStructuralError(err, &structural) && structural.Code == StaleGeneration {
		t.Fatalf("an uninstalled generation was blamed on the token: %v", err)
	}
}

// A withdrawal names who is withdrawing and what is being withdrawn. Neither
// is defaultable: a cancel with no waiter would be a request to remove
// something unspecified from a shared queue, and a cancel with no actor
// leaves the audit record unable to say who did it.
func TestAWithdrawalWithoutAnActorOrAWaiterIsRefused(t *testing.T) {
	s := refusalHeld(t)
	s, _ = applyForTest(t, s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-2", LeaseID: "l-2", Holder: "agent-b", Class: ClassAI,
		Reason: "queued work", Duration: time.Hour,
	}}, refusalAt(time.Minute))

	for _, bad := range []struct {
		name    string
		command CancelWaiter
	}{
		{name: "a cancel with no actor", command: CancelWaiter{WaiterID: "w-2"}},
		{name: "a cancel with no waiter", command: CancelWaiter{Actor: "server"}},
		{name: "a cancel with neither", command: CancelWaiter{}},
	} {
		after, _, err := Apply(s, bad.command, refusalAt(2*time.Minute))
		refusalFiled(t, err, InvalidArgument, bad.name)
		if len(after.Queue) != len(s.Queue) {
			t.Fatalf("%s changed the queue from %d to %d", bad.name, len(s.Queue), len(after.Queue))
		}
	}

	// The complete request is honoured, so the refusals above are about the
	// missing fields and not about the queue's state.
	after, _ := applyForTest(t, s, CancelWaiter{Actor: "server", WaiterID: "w-2"}, refusalAt(2*time.Minute))
	if len(after.Queue) != 0 {
		t.Fatalf("a complete withdrawal left %d queued", len(after.Queue))
	}
}

// A yield request asks a live holder to stop early, so the identity asking
// for it is part of the request. An unattributed yield is the one shape that
// would let a handoff be demanded with nothing on record to answer for it.
func TestAYieldRequestWithoutAnActorIsRefused(t *testing.T) {
	s := refusalHeld(t)

	after, _, err := Apply(s, RequestYield{WaiterID: "w-2"}, refusalAt(time.Minute))
	refusalFiled(t, err, InvalidArgument, "a yield request with no actor")
	if after.Phase != Active {
		t.Fatalf("an unattributed yield moved the board to %v", after.Phase)
	}
}

// An operator finishing a recovery reports what the board agent durably
// holds. A number below what the server already witnessed is not a recovery
// that can be accepted: it is evidence the agent's state went backwards, so
// the board is quarantined instead of returned to service. The refusal is a
// transition rather than an error, because the board really did change.
func TestARecoveryThatRegressesTheAgentHighWaterQuarantinesTheBoard(t *testing.T) {
	s := refusalHeld(t)
	s, _ = applyForTest(t, s, Quarantine{Actor: "operator", Reason: "fixture suspect"}, refusalAt(time.Minute))
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "plan-7",
		Reason: "reseat the fixture"}, refusalAt(2*time.Minute))
	if s.Phase != Recovering {
		t.Fatalf("board is %v, not recovering", s.Phase)
	}

	// The agent reported a high-water mark ahead of the server's generation
	// during the recovery, which a recovering board is allowed to carry.
	witnessed := clone(s)
	witnessed.AgentHighWater = s.Generation + 4
	if err := Validate(witnessed); err != nil {
		t.Fatalf("a recovering board may carry an agent ahead of the server: %v", err)
	}

	after, events := applyForTest(t, witnessed, CompleteRecovery{Actor: "operator",
		NeutralReceipt: "receipt-1", AgentHighWater: witnessed.AgentHighWater - 1}, refusalAt(3*time.Minute))
	if after.Phase != Quarantined {
		t.Fatalf("a regressed high-water mark left the board %v", after.Phase)
	}
	// The lease is retained rather than dropped. A quarantine is a hold for
	// an operator, not a release: whoever held the board when its state went
	// backwards stays on the record, and only a reviewed recovery clears it.
	if after.Lease == nil || after.Lease.ID != witnessed.Lease.ID {
		t.Fatalf("the quarantine dropped the lease on record: %+v", after.Lease)
	}

	var quarantined bool
	for _, e := range events {
		if e.Kind == BoardQuarantined {
			quarantined = true
			if e.Reason == "" {
				t.Fatal("the quarantine was filed with no reason")
			}
		}
		if e.Kind == RecoveryFinished {
			t.Fatalf("a regressed recovery was recorded as finished: %+v", e)
		}
	}
	if !quarantined {
		t.Fatal("no quarantine event was filed for a regressed high-water mark")
	}

	// The same receipt with the witnessed mark intact completes the
	// recovery, so the quarantine above is about the regression and not
	// about the receipt or the phase.
	restored, _ := applyForTest(t, witnessed, CompleteRecovery{Actor: "operator",
		NeutralReceipt: "receipt-1", AgentHighWater: witnessed.AgentHighWater}, refusalAt(3*time.Minute))
	if restored.Phase == Quarantined {
		t.Fatal("an honest recovery was quarantined")
	}
}

// The grant path checks the same disagreement from the other side. Validate
// refuses an agent ahead of the server outside quarantine and recovery, so a
// board in this shape cannot reach Apply: the guard is reached only by
// calling the granter directly, which is what this does. It is defence in
// depth for a restored database, where the server's generation counter has
// gone back in time and the next grant would reuse a generation the agent has
// already installed.
func TestNoGrantIsMadeWhileTheAgentIsAheadOfTheDatabase(t *testing.T) {
	s := boardForTest(t)
	s.Queue = []Waiter{{ID: "w-1", LeaseID: "l-1", Holder: "agent-a", Class: ClassAI,
		Reason: "experiment", Duration: time.Hour, QueuedAt: refusalAt(0), Sequence: 1}}
	s.Generation = 3
	s.AgentHighWater = 4

	events := make([]Event, 0, 1)
	err := grantNext(&s, refusalAt(time.Minute), "server", &events)
	refusalFiled(t, err, RecoveryNecessary, "a grant against a restored database")
	if s.Lease != nil {
		t.Fatalf("a lease was granted against a restored database: %+v", s.Lease)
	}
	if len(events) != 0 {
		t.Fatalf("a refused grant filed %d events", len(events))
	}

	// Level with the agent, the same queue is granted, so the refusal is the
	// regression and not the waiter.
	s.AgentHighWater = 3
	if err := grantNext(&s, refusalAt(time.Minute), "server", &events); err != nil {
		t.Fatalf("a level board refused its queue: %v", err)
	}
	if s.Lease == nil {
		t.Fatal("a level board granted nothing")
	}
}
