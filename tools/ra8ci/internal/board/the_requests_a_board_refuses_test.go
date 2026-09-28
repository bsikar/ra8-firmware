package board

import (
	"math"
	"testing"
	"time"
)

// The refusals on the two doors a grant passes through: enqueue, where a
// request asks to join, and acknowledge, where the agent claims the grant it
// was given. Both doors accept an exact repeat as a no-op and refuse anything
// that merely resembles one, and the distinction is what keeps a retry from
// being read as a second request or a second install.

func joinAt(offset time.Duration) time.Time { return testEpoch.Add(offset) }

// joinWaiter is a complete, admissible request; callers vary one field.
func joinWaiter(id, leaseID string, class Class) Waiter {
	return Waiter{ID: id, LeaseID: leaseID, Holder: "agent-" + id, Class: class, Reason: "work " + id, Duration: 30 * time.Minute}
}

// joinBoard is a board held by w-1/l-1 (agent-a, AI, "experiment", one hour),
// the grant heldBoard builds, acknowledged and active.
func joinBoard(t *testing.T) Snapshot {
	t.Helper()
	return heldBoard(t, testEpoch, time.Hour)
}

// pendingBoard is a board whose grant has been created and not yet
// acknowledged, which is the only phase an acknowledgement can land in.
func pendingBoard(t *testing.T) Snapshot {
	t.Helper()
	s, err := New("board-join")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	s, _, err = Apply(s, Enqueue{Actor: "server", Waiter: joinWaiter("w-1", "l-1", ClassAI)}, testEpoch)
	if err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	if s.Phase != GrantPending {
		t.Fatalf("grant not pending: %v", s.Phase)
	}
	return s
}

func refused(t *testing.T, err error, code Code, detail string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected %s: %s, got no error", code, detail)
	}
	if !IsCode(err, code) {
		t.Fatalf("expected code %s, got %v", code, err)
	}
	var boardErr *Error
	if !asBoardError(err, &boardErr) || boardErr.Detail != detail {
		t.Fatalf("expected detail %q, got %v", detail, err)
	}
}

func asBoardError(err error, target **Error) bool {
	if e, ok := err.(*Error); ok {
		*target = e
		return true
	}
	return false
}

// --- enqueue: a request that cannot join -----------------------------------

// A retry that reuses the held request's ID but asks for something else is
// not the held request. Admitting it would put the holder's own ID in the
// queue behind it, and the audit trail would then carry one ID for two
// different pieces of work.
func TestARequestReusingTheHeldWaiterIDIsRefused(t *testing.T) {
	s := joinBoard(t)
	other := joinWaiter("w-1", "l-9", ClassCI)

	after, events, err := Apply(s, Enqueue{Actor: "server", Waiter: other}, joinAt(time.Minute))
	refused(t, err, Conflict, "request or lease ID belongs to current grant")
	if len(after.Queue) != 0 {
		t.Fatalf("refused request joined the queue: %+v", after.Queue)
	}
	if after.Phase != Active || after.Lease.ID != "l-1" {
		t.Fatalf("holder disturbed: phase %v lease %+v", after.Phase, after.Lease)
	}
	if len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("expected one denial event, got %+v", events)
	}
}

// The same refusal from the other side: a fresh request ID carrying the
// held lease ID.
func TestARequestReusingTheHeldLeaseIDIsRefused(t *testing.T) {
	s := joinBoard(t)
	other := joinWaiter("w-9", "l-1", ClassCI)

	after, _, err := Apply(s, Enqueue{Actor: "server", Waiter: other}, joinAt(time.Minute))
	refused(t, err, Conflict, "request or lease ID belongs to current grant")
	if len(after.Queue) != 0 {
		t.Fatalf("refused request joined the queue: %+v", after.Queue)
	}
}

// The exact request that won the board is idempotent, so a caller that
// retries after a lost response does not queue behind itself.
func TestAnExactRepeatOfTheHeldRequestChangesNothing(t *testing.T) {
	s := joinBoard(t)
	repeat := Waiter{ID: "w-1", LeaseID: "l-1", Holder: "agent-a", Class: ClassAI, Reason: "experiment", Duration: time.Hour}

	after, events, err := Apply(s, Enqueue{Actor: "server", Waiter: repeat}, joinAt(time.Minute))
	if err != nil {
		t.Fatalf("exact repeat refused: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("repeat filed events: %+v", events)
	}
	if len(after.Queue) != 0 || after.Version != s.Version || after.Phase != Active {
		t.Fatalf("repeat moved the board: queue %d version %d phase %v", len(after.Queue), after.Version, after.Phase)
	}
}

// Two queued requests may not share a lease ID: the lease ID is what the
// grant is issued under, so a second waiter holding it would be handed a
// grant the first one's holder can present a token for.
func TestTwoQueuedRequestsCannotShareALeaseID(t *testing.T) {
	s := joinBoard(t)
	s, _, err := Apply(s, Enqueue{Actor: "server", Waiter: joinWaiter("w-2", "l-2", ClassAI)}, joinAt(time.Minute))
	if err != nil {
		t.Fatalf("first waiter refused: %v", err)
	}

	clash := joinWaiter("w-3", "l-2", ClassAI)
	after, _, err := Apply(s, Enqueue{Actor: "server", Waiter: clash}, joinAt(2*time.Minute))
	refused(t, err, Conflict, "lease ID collision")
	if len(after.Queue) != 1 || after.Queue[0].ID != "w-2" {
		t.Fatalf("queue disturbed: %+v", after.Queue)
	}
	if after.NextSequence != s.NextSequence {
		t.Fatalf("refused request spent a sequence: %d -> %d", s.NextSequence, after.NextSequence)
	}
}

// The request ID is judged before the lease ID, so a caller that changed its
// mind about the work is told its request ID collided rather than being sent
// looking at a lease ID it did reuse deliberately.
func TestAQueuedRequestIDCollisionKeepsItsOwnRefusal(t *testing.T) {
	s := joinBoard(t)
	s, _, err := Apply(s, Enqueue{Actor: "server", Waiter: joinWaiter("w-2", "l-2", ClassAI)}, joinAt(time.Minute))
	if err != nil {
		t.Fatalf("first waiter refused: %v", err)
	}

	differing := joinWaiter("w-2", "l-2", ClassCI)
	_, _, err = Apply(s, Enqueue{Actor: "server", Waiter: differing}, joinAt(2*time.Minute))
	refused(t, err, Conflict, "request ID collision")
}

// A queue whose sequence counter cannot advance refuses the request outright
// rather than appending a waiter whose arrival order is unrecordable.
func TestAQueueWhoseSequenceCannotAdvanceRefusesTheRequest(t *testing.T) {
	s, err := New("board-join")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	s.NextSequence = math.MaxUint64

	after, events, err := Apply(s, Enqueue{Actor: "server", Waiter: joinWaiter("w-2", "l-2", ClassAI)}, testEpoch)
	refused(t, err, Conflict, "queue sequence exhausted")
	if len(after.Queue) != 0 || after.Lease != nil || after.Phase != Ready {
		t.Fatalf("board moved on a refused request: %+v", after)
	}
	if after.NextSequence != math.MaxUint64 {
		t.Fatalf("sequence wrapped: %d", after.NextSequence)
	}
	if len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("expected one denial event, got %+v", events)
	}
}

// --- acknowledge: a claim that cannot land ---------------------------------

// The identity guard runs ahead of the grant check, so an agent that sent no
// name is told that rather than that its grant went stale.
func TestAnAcknowledgementWithNoAgentIdentityIsRefused(t *testing.T) {
	s := pendingBoard(t)

	_, _, err := Apply(s, AcknowledgeGrant{LeaseID: "l-1", Generation: s.Generation, InstalledGeneration: s.Generation}, joinAt(time.Minute))
	refused(t, err, InvalidArgument, "missing board-agent identity")

	// Nameless AND stale still reads as the missing name.
	_, _, err = Apply(s, AcknowledgeGrant{LeaseID: "l-gone", Generation: 99, InstalledGeneration: 99}, joinAt(time.Minute))
	refused(t, err, InvalidArgument, "missing board-agent identity")
}

// An acknowledgement carries the grant check's own verdict, so an agent
// holding a superseded token learns the token is stale rather than that the
// phase is wrong.
func TestAnAcknowledgementOfAGrantThatIsNotCurrentIsRefused(t *testing.T) {
	s := pendingBoard(t)
	gen := s.Generation

	_, _, err := Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-other", Generation: gen, InstalledGeneration: gen}, joinAt(time.Minute))
	refused(t, err, StaleGeneration, "lease token is stale")

	_, _, err = Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: gen + 1, InstalledGeneration: gen + 1}, joinAt(time.Minute))
	refused(t, err, StaleGeneration, "lease token is stale")

	_, _, err = Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: 0, InstalledGeneration: 0}, joinAt(time.Minute))
	refused(t, err, StaleGeneration, "no matching grant")
}

// A grant that ran out before the acknowledgement arrived is expired by Apply
// first, and the expiry keeps the lease as evidence, so the grant check is
// what refuses and the agent is told its lease expired. That reading matters:
// an agent that installed a generation and then lost the race to report it
// learns the deadline passed, not that its token was never real, and the
// board records the expiry before the denial either way. The expiry is exact
// at the instant named, so one nanosecond earlier the acknowledgement lands.
func TestAnAcknowledgementAfterTheDeadlineIsRefusedAsExpired(t *testing.T) {
	s := pendingBoard(t)
	gen := s.Generation
	ack := AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: gen, InstalledGeneration: gen}

	after, events, err := Apply(s, ack, s.Lease.ExpiresAt)
	refused(t, err, Expired, "lease has expired")
	if after.Phase != RecoveryRequired {
		t.Fatalf("expired board did not need recovery: %v", after.Phase)
	}
	if after.Lease == nil {
		t.Fatalf("expiry dropped the lease that records whose work was cut short")
	}
	if len(events) != 2 || events[0].Kind != LeaseExpired || events[1].Kind != ActionDenied {
		t.Fatalf("expected expiry then denial, got %+v", events)
	}

	intime, _, err := Apply(s, ack, s.Lease.ExpiresAt.Add(-time.Nanosecond))
	if err != nil {
		t.Fatalf("acknowledgement one nanosecond inside the deadline refused: %v", err)
	}
	if intime.Phase != Active {
		t.Fatalf("in-time acknowledgement did not activate the board: %v", intime.Phase)
	}
}

// A repeat of the acknowledgement the board already accepted changes nothing
// at all: no phase move, no event, no version. A board agent that retries
// after a lost response must not look like a second install.
func TestARepeatedAcknowledgementChangesNothing(t *testing.T) {
	s := joinBoard(t)
	gen := s.Generation

	after, events, err := Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: gen, InstalledGeneration: gen}, joinAt(time.Minute))
	if err != nil {
		t.Fatalf("repeat acknowledgement refused: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("repeat filed events: %+v", events)
	}
	if after.Phase != Active || after.Version != s.Version || after.AgentHighWater != s.AgentHighWater {
		t.Fatalf("repeat moved the board: phase %v version %d high water %d", after.Phase, after.Version, after.AgentHighWater)
	}
}

// The repeat is accepted in every phase where the grant is still running, not
// only the one it was acknowledged in: a yield has been asked for, or a drain
// has started, and the agent's retry still describes the install that happened.
func TestTheRepeatIsAcceptedInEveryPhaseTheGrantStillRuns(t *testing.T) {
	base := joinBoard(t)
	gen := base.Generation

	yielding, _, err := Apply(base, Enqueue{Actor: "server", Waiter: joinWaiter("w-h", "l-h", ClassHuman)}, joinAt(time.Minute))
	if err != nil {
		t.Fatalf("human waiter refused: %v", err)
	}
	if yielding.Phase != YieldRequested {
		t.Fatalf("board was not asked to yield: %v", yielding.Phase)
	}
	// A drain only starts once a yield has been asked for, so the draining
	// board is the yielding one taken one step further.
	draining, _, err := Apply(yielding, BeginDrain{Actor: "agent-a", LeaseID: "l-1", Generation: gen}, joinAt(90*time.Second))
	if err != nil {
		t.Fatalf("drain refused: %v", err)
	}
	if draining.Phase != Draining {
		t.Fatalf("board is not draining: %v", draining.Phase)
	}

	for _, running := range []Snapshot{base, yielding, draining} {
		after, events, err := Apply(running, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: gen, InstalledGeneration: gen}, joinAt(2*time.Minute))
		if err != nil {
			t.Fatalf("repeat refused in %v: %v", running.Phase, err)
		}
		if len(events) != 0 || after.Phase != running.Phase || after.Version != running.Version {
			t.Fatalf("repeat moved a %v board: phase %v events %+v", running.Phase, after.Phase, events)
		}
	}
}

// An acknowledgement naming a generation the board never granted is not the
// idempotent repeat, so it falls to the phase check and is refused there. The
// board is live and nothing is pending: there is no grant to install.
func TestAnAcknowledgementOfAnotherGenerationOnALiveBoardIsRefused(t *testing.T) {
	s := joinBoard(t)
	gen := s.Generation

	after, _, err := Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: gen, InstalledGeneration: gen - 1}, joinAt(time.Minute))
	refused(t, err, Conflict, "grant is not pending")
	if after.Phase != Active || after.AgentHighWater != s.AgentHighWater {
		t.Fatalf("refused acknowledgement moved the board: phase %v high water %d", after.Phase, after.AgentHighWater)
	}
}

// A board in recovery retains the interrupted lease as evidence, so the grant
// check passes and the phase check is what refuses. The agent is told the
// grant is not pending rather than that its token is stale, because the token
// is genuinely the current one.
func TestAnAcknowledgementAgainstABoardInRecoveryIsRefused(t *testing.T) {
	s := joinBoard(t)
	gen := s.Generation
	s, _, err := Apply(s, AgentUnavailable{Actor: "monitor", Reason: "heartbeat lost"}, joinAt(time.Minute))
	if err != nil {
		t.Fatalf("agent unavailable refused: %v", err)
	}
	if s.Phase != RecoveryRequired || s.Lease == nil {
		t.Fatalf("expected a retained lease awaiting recovery: phase %v lease %+v", s.Phase, s.Lease)
	}

	after, _, err := Apply(s, AcknowledgeGrant{Actor: "board-agent", LeaseID: "l-1", Generation: gen, InstalledGeneration: gen}, joinAt(2*time.Minute))
	refused(t, err, Conflict, "grant is not pending")
	if after.Phase != RecoveryRequired || after.Lease == nil {
		t.Fatalf("recovery state disturbed: phase %v lease %+v", after.Phase, after.Lease)
	}
}
