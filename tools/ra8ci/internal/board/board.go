// Package board implements the deterministic, hardware-free board lease state
// machine. Only the trusted server may call Apply: it must authenticate Actor,
// authorize the command against the board and generation, and verify neutral
// receipts against the board, lease generation, and approved fixture profile.
// A nonempty receipt string alone is not proof. The server must commit a
// returned snapshot and its events in one database transaction before acting.
package board

import (
	"errors"
	"fmt"
	"math"
	"time"
)

// Class is a board waiter's priority. A larger value has higher priority.
type Class uint8

const (
	ClassAI Class = iota + 1
	ClassCI
	ClassHuman
)

// MaxWaiters bounds the durable in-memory queue for one board.
const MaxWaiters = 1024

// Phase is the board's durable grant and recovery state.
type Phase string

const (
	Ready            Phase = "ready"
	GrantPending     Phase = "grant_pending"
	Active           Phase = "active"
	YieldRequested   Phase = "yield_requested"
	Draining         Phase = "draining"
	RecoveryRequired Phase = "recovery_required"
	Recovering       Phase = "recovering"
	Quarantined      Phase = "quarantined"
)

// Code identifies a machine-readable board transition failure.
type Code string

const (
	InvalidArgument   Code = "invalid_argument"
	Conflict          Code = "conflict"
	StaleGeneration   Code = "stale_generation"
	Expired           Code = "expired"
	RecoveryNecessary Code = "recovery_required"
	Denied            Code = "denied"
	Deadline          Code = "deadline"
)

// Error is returned for a rejected transition. Rejection is also emitted as
// an audit event, even when an expiry transition occurred first.
type Error struct {
	Code   Code
	Detail string
}

func (e *Error) Error() string { return string(e.Code) + ": " + e.Detail }

// IsCode reports whether err is a board error with the requested code.
func IsCode(err error, code Code) bool {
	var boardErr *Error
	return errors.As(err, &boardErr) && boardErr.Code == code
}

// Waiter is a durable queue entry. ID and LeaseID are distinct caller-issued
// opaque IDs; exact duplicate requests are idempotent.
type Waiter struct {
	ID       string
	LeaseID  string
	Holder   string
	Class    Class
	Reason   string
	Duration time.Duration
	Sequence uint64
	QueuedAt time.Time
}

// Lease is the sole pending or active grant for a board.
type Lease struct {
	ID                     string
	WaiterID               string
	Holder                 string
	Class                  Class
	Reason                 string
	Generation             uint64
	GrantedAt              time.Time
	ExpiresAt              time.Time
	RequestedDuration      time.Duration
	DeadlineVersion        uint64
	YieldRequestedAt       time.Time
	ContendedExtensionUsed time.Duration

	// LastHeartbeatAt is when the holder last reported itself alive. Zero
	// means it has not reported since the grant, which the grant itself
	// already witnesses. It moves only forward, never lengthens or shortens
	// ExpiresAt, and is read by ObserveHolderLiveness; silence is evidence a
	// holder may have crashed, never authority withdrawn from it.
	LastHeartbeatAt time.Time

	// HandoffTarget is the handoff ETA the requester was shown when this
	// board was asked to yield, retained for as long as the request is
	// outstanding. It is a promise already made, not an estimate: a later
	// re-estimate over more history must not move the number a waiter was
	// given, and the sample recorded when the handoff ends is judged against
	// this. Zero means no ETA was shown, which is the honest answer for a
	// yield the state machine raised itself and for a task that declares no
	// handoff bounds.
	HandoffTarget time.Duration

	// HandoffCohort is the comparable history the shown target was estimated
	// over, retained alongside it for the same reason. The sample recorded
	// when this handoff ends must land in the bucket whose estimate the
	// requester was actually shown; a cohort re-derived at completion time
	// would file the measurement against whatever the board is doing by
	// then, which is how an estimate ends up judged by work it never
	// described. A zero cohort means none was recorded.
	HandoffCohort YieldCohort
}

// Snapshot is a copyable board state. The store must serialize updates by
// board ID and compare Version before committing a replacement.
type Snapshot struct {
	BoardID        string
	Phase          Phase
	Generation     uint64
	AgentHighWater uint64
	Version        uint64
	NextSequence   uint64
	Lease          *Lease
	Queue          []Waiter
}

// New creates an empty ready board state.
func New(boardID string) (Snapshot, error) {
	if boardID == "" {
		return Snapshot{}, &Error{InvalidArgument, "board ID is empty"}
	}
	return Snapshot{BoardID: boardID, Phase: Ready}, nil
}

// EventKind names an append-only audit/event record.
type EventKind string

const (
	WaitQueued           EventKind = "wait_queued"
	WaitCancelled        EventKind = "wait_cancelled"
	GrantCreated         EventKind = "grant_pending"
	GrantAcknowledged    EventKind = "grant_acknowledged"
	YieldAsked           EventKind = "yield_requested"
	YieldCleared         EventKind = "yield_cleared"
	DrainStarted         EventKind = "drain_started"
	LeaseReleased        EventKind = "lease_released"
	LeaseExtended        EventKind = "lease_extended"
	LeaseExpired         EventKind = "lease_expired"
	RecoveryNeeded       EventKind = "recovery_required"
	RecoveryStarted      EventKind = "recovery_started"
	RecoveryFinished     EventKind = "recovery_finished"
	BoardQuarantined     EventKind = "quarantined"
	AgentObserved        EventKind = "agent_generation_observed"
	GenerationReconciled EventKind = "generation_reconciled"
	ActionDenied         EventKind = "action_denied"
)

// Event is emitted for every state-changing transition and denied command.
// Actor is the authenticated identity passed by the caller, not a request-body
// assertion; authorization remains the server's responsibility.
type Event struct {
	Kind       EventKind
	At         time.Time
	BoardID    string
	Actor      string
	WaiterID   string
	LeaseID    string
	Generation uint64
	Reason     string
}

// Command is a typed request to transition one board snapshot.
type Command interface{ boardCommand() }

// Enqueue adds a request to the priority/FIFO wait queue.
type Enqueue struct {
	Actor  string
	Waiter Waiter
}

func (Enqueue) boardCommand() {}

// CancelWaiter withdraws a queued request; authorization belongs to the server.
type CancelWaiter struct {
	Actor    string
	WaiterID string
}

func (CancelWaiter) boardCommand() {}

// AcknowledgeGrant confirms that the board agent durably installed Generation.
type AcknowledgeGrant struct {
	Actor               string
	LeaseID             string
	Generation          uint64
	InstalledGeneration uint64
}

func (AcknowledgeGrant) boardCommand() {}

// RequestYield asks the current holder to stop at its next safe checkpoint.
// The named queued waiter must outrank the holder.
type RequestYield struct {
	Actor    string
	WaiterID string

	// ShownTarget is the handoff ETA the requester was shown by PlanYield
	// before this command was issued. It is recorded on the lease so the
	// promise survives the request. Zero states that no ETA was shown.
	ShownTarget time.Duration

	// Cohort is the history ShownTarget was estimated over, recorded with
	// it so the sample this handoff leaves behind is filed against the same
	// comparable work. It is required whenever ShownTarget is nonzero: a
	// promise with no cohort behind it cannot be measured against anything.
	Cohort YieldCohort
}

func (RequestYield) boardCommand() {}

// BeginDrain records that the holder has reached a safe checkpoint and begun
// neutralization. It does not release the board.
type BeginDrain struct {
	Actor      string
	LeaseID    string
	Generation uint64
}

func (BeginDrain) boardCommand() {}

// Release ends a lease. The trusted server must verify NeutralReceipt against
// the current board, generation, and fixture profile before calling Apply. An
// empty receipt requires recovery; a merely nonempty string proves nothing.
type Release struct {
	Actor          string
	LeaseID        string
	Generation     uint64
	NeutralReceipt string
}

func (Release) boardCommand() {}

// Extend moves a live lease deadline forward, subject to class and contention
// ceilings. The caller separately verifies extension authority and rationale.
type Extend struct {
	Actor      string
	LeaseID    string
	Generation uint64
	NewExpiry  time.Time
	Reason     string
}

func (Extend) boardCommand() {}

// Tick checks expiry and grants the next waiter only if the board is ready.
type Tick struct{ Actor string }

func (Tick) boardCommand() {}

// AgentUnavailable prevents further operations when the board-side agent's
// heartbeat or monotonic-clock continuity is lost.
type AgentUnavailable struct {
	Actor  string
	Reason string
}

func (AgentUnavailable) boardCommand() {}

// BeginRecovery starts an operator-approved recovery plan.
type BeginRecovery struct {
	Actor  string
	PlanID string
	Reason string
}

func (BeginRecovery) boardCommand() {}

// CompleteRecovery accepts a neutral receipt already authenticated and bound
// by the server to the board, generation, and approved fixture profile.
type CompleteRecovery struct {
	Actor          string
	NeutralReceipt string
	AgentHighWater uint64
}

func (CompleteRecovery) boardCommand() {}

// Quarantine immediately blocks grants and board activity pending recovery.
type Quarantine struct {
	Actor  string
	Reason string
}

func (Quarantine) boardCommand() {}

// ObserveAgentGeneration compares the agent's durable high-water mark with
// server state. A restored database behind the agent is quarantined.
type ObserveAgentGeneration struct {
	Actor     string
	HighWater uint64
}

func (ObserveAgentGeneration) boardCommand() {}

// Apply produces one deterministic state transition. This is a server-only
// API: the caller must authenticate Actor, authorize the requested board and
// generation, and verify any neutral receipt against an approved fixture
// profile before invoking it. It must commit the returned snapshot and all
// events atomically, including when err is non-nil: expiry and denial may both
// be recorded in that result. No hardware or I/O is performed here.
func Apply(before Snapshot, command Command, now time.Time) (Snapshot, []Event, error) {
	s := clone(before)
	if s.BoardID == "" || now.IsZero() || command == nil {
		return before, nil, &Error{InvalidArgument, "missing board, command, or time"}
	}
	if err := Validate(s); err != nil {
		return before, nil, err
	}
	events := make([]Event, 0, 3)
	changed := expire(&s, now, &events)
	afterExpiry := clone(s)
	expiryEvents := len(events)
	actor := commandActor(command)
	var err error
	switch c := command.(type) {
	case Enqueue:
		err = enqueue(&s, c, now, &events)
	case CancelWaiter:
		err = cancel(&s, c, now, &events)
	case AcknowledgeGrant:
		err = acknowledge(&s, c, now, &events)
	case RequestYield:
		err = requestYield(&s, c, now, &events)
	case BeginDrain:
		err = beginDrain(&s, c, now, &events)
	case Release:
		err = release(&s, c, now, &events)
	case Extend:
		err = extend(&s, c, now, &events)
	case HolderHeartbeat:
		var beat bool
		beat, err = holderHeartbeat(&s, c, now)
		changed = changed || beat
	case Tick:
		err = grantNext(&s, now, c.Actor, &events)
	case AgentUnavailable:
		err = agentUnavailable(&s, c, now, &events)
	case BeginRecovery:
		err = beginRecovery(&s, c, now, &events)
	case CompleteRecovery:
		err = completeRecovery(&s, c, now, &events)
	case Quarantine:
		err = quarantine(&s, c.Actor, c.Reason, now, &events)
	case ObserveAgentGeneration:
		err = observeGeneration(&s, c, now, &events)
	default:
		err = &Error{InvalidArgument, "unknown command"}
	}
	if err != nil {
		s = afterExpiry
		events = events[:expiryEvents]
		events = append(events, event(&s, ActionDenied, now, actor, "", "", err.Error()))
	}
	if changed || len(events) > 0 {
		if s.Version == math.MaxUint64 {
			return before, nil, &Error{Conflict, "snapshot version exhausted"}
		}
		s.Version++
	}
	return s, events, err
}

// Token is the generation-fenced capability a board agent must check before
// every new board step. The nonce is represented by the opaque lease ID.
type Token struct {
	BoardID    string
	LeaseID    string
	Generation uint64
}

// CanStartSegment checks server-side lease policy. The board agent must also
// check its durable high-water mark, locally seeded monotonic deadline, and
// its serialized hardware gate before starting a segment.
func CanStartSegment(s Snapshot, token Token, now time.Time, bound, recoveryMargin time.Duration) error {
	if bound <= 0 || recoveryMargin < 0 || now.IsZero() {
		return &Error{InvalidArgument, "invalid segment bound, margin, or time"}
	}
	if s.Lease == nil || s.Phase != Active {
		return &Error{RecoveryNecessary, "board is not active for a new segment"}
	}
	if token.BoardID != s.BoardID || token.LeaseID != s.Lease.ID || token.Generation != s.Lease.Generation {
		return &Error{StaleGeneration, "lease token does not match the active grant"}
	}
	if s.AgentHighWater != s.Lease.Generation {
		return &Error{RecoveryNecessary, "board agent has not installed the current generation"}
	}
	if !now.Before(s.Lease.ExpiresAt) {
		return &Error{Expired, "lease has expired"}
	}
	if higherWaiting(s) {
		return &Error{Denied, "higher-priority waiter is queued"}
	}
	if bound > s.Lease.ExpiresAt.Sub(now)-recoveryMargin {
		return &Error{Deadline, "insufficient lease time for segment and recovery"}
	}
	return nil
}

func clone(s Snapshot) Snapshot {
	if s.Lease != nil {
		copyLease := *s.Lease
		s.Lease = &copyLease
	}
	s.Queue = append([]Waiter(nil), s.Queue...)
	return s
}

func event(s *Snapshot, kind EventKind, at time.Time, actor, waiterID, leaseID, reason string) Event {
	return Event{Kind: kind, At: at, BoardID: s.BoardID, Actor: actor, WaiterID: waiterID, LeaseID: leaseID, Generation: s.Generation, Reason: reason}
}

func commandActor(command Command) string {
	switch c := command.(type) {
	case Enqueue:
		return c.Actor
	case CancelWaiter:
		return c.Actor
	case AcknowledgeGrant:
		return c.Actor
	case RequestYield:
		return c.Actor
	case BeginDrain:
		return c.Actor
	case Release:
		return c.Actor
	case Extend:
		return c.Actor
	case HolderHeartbeat:
		return c.Actor
	case Tick:
		return c.Actor
	case AgentUnavailable:
		return c.Actor
	case BeginRecovery:
		return c.Actor
	case CompleteRecovery:
		return c.Actor
	case Quarantine:
		return c.Actor
	case ObserveAgentGeneration:
		return c.Actor
	default:
		return ""
	}
}

func validClass(class Class) bool { return class == ClassAI || class == ClassCI || class == ClassHuman }

func classCeiling(class Class) time.Duration {
	switch class {
	case ClassAI:
		return time.Hour
	case ClassCI:
		return 2 * time.Hour
	case ClassHuman:
		return 8 * time.Hour
	default:
		return 0
	}
}

func enqueue(s *Snapshot, c Enqueue, now time.Time, events *[]Event) error {
	w := c.Waiter
	if c.Actor == "" || w.ID == "" || w.LeaseID == "" || w.Holder == "" || w.Reason == "" || !validClass(w.Class) || w.Duration <= 0 || w.Duration > classCeiling(w.Class) {
		return &Error{InvalidArgument, "invalid board request"}
	}
	if s.Lease != nil && (w.ID == s.Lease.WaiterID || w.LeaseID == s.Lease.ID) {
		if sameLeaseRequest(*s.Lease, w) {
			return nil
		}
		return &Error{Conflict, "request or lease ID belongs to current grant"}
	}
	for _, existing := range s.Queue {
		if existing.ID == w.ID {
			if sameWaiter(existing, w) {
				return nil
			}
			return &Error{Conflict, "request ID collision"}
		}
		if existing.LeaseID == w.LeaseID {
			return &Error{Conflict, "lease ID collision"}
		}
	}
	if len(s.Queue) >= MaxWaiters {
		return &Error{Denied, "board wait queue is full"}
	}
	if s.NextSequence == math.MaxUint64 {
		return &Error{Conflict, "queue sequence exhausted"}
	}
	s.NextSequence++
	w.Sequence = s.NextSequence
	w.QueuedAt = now
	s.Queue = append(s.Queue, w)
	*events = append(*events, event(s, WaitQueued, now, c.Actor, w.ID, w.LeaseID, w.Reason))
	if s.Phase == Active && s.Lease != nil && w.Class > s.Lease.Class {
		s.Phase = YieldRequested
		s.Lease.YieldRequestedAt = now
		*events = append(*events, event(s, YieldAsked, now, c.Actor, w.ID, s.Lease.ID, "higher-priority waiter"))
	}
	return grantNext(s, now, c.Actor, events)
}

func sameWaiter(a, b Waiter) bool {
	return a.ID == b.ID && a.LeaseID == b.LeaseID && a.Holder == b.Holder && a.Class == b.Class && a.Reason == b.Reason && a.Duration == b.Duration
}

func sameLeaseRequest(lease Lease, w Waiter) bool {
	return lease.WaiterID == w.ID && lease.ID == w.LeaseID && lease.Holder == w.Holder && lease.Class == w.Class && lease.Reason == w.Reason && lease.RequestedDuration == w.Duration
}

func cancel(s *Snapshot, c CancelWaiter, now time.Time, events *[]Event) error {
	if c.Actor == "" || c.WaiterID == "" {
		return &Error{InvalidArgument, "missing actor or waiter ID"}
	}
	for i, w := range s.Queue {
		if w.ID == c.WaiterID {
			s.Queue = append(s.Queue[:i], s.Queue[i+1:]...)
			*events = append(*events, event(s, WaitCancelled, now, c.Actor, w.ID, w.LeaseID, "withdrawn"))
			if s.Phase == YieldRequested && !higherWaiting(*s) {
				s.Phase = Active
				s.Lease.YieldRequestedAt = time.Time{}
				s.Lease.HandoffTarget = 0
				s.Lease.HandoffCohort = YieldCohort{}
				*events = append(*events, event(s, YieldCleared, now, c.Actor, w.ID, s.Lease.ID, "no higher-priority waiter remains"))
			}
			return nil
		}
	}
	return &Error{Conflict, "waiter is not queued"}
}

func acknowledge(s *Snapshot, c AcknowledgeGrant, now time.Time, events *[]Event) error {
	if c.Actor == "" {
		return &Error{InvalidArgument, "missing board-agent identity"}
	}
	if err := current(s, c.LeaseID, c.Generation, now); err != nil {
		return err
	}
	if (s.Phase == Active || s.Phase == YieldRequested || s.Phase == Draining) && c.InstalledGeneration == c.Generation && s.AgentHighWater == c.Generation {
		return nil
	}
	if s.Phase != GrantPending {
		return &Error{Conflict, "grant is not pending"}
	}
	if c.InstalledGeneration > s.Generation {
		s.AgentHighWater = c.InstalledGeneration
		return quarantine(s, c.Actor, "agent generation exceeds database generation", now, events)
	}
	if c.InstalledGeneration != c.Generation {
		return &Error{StaleGeneration, "agent did not persist current generation"}
	}
	s.AgentHighWater = c.InstalledGeneration
	s.Phase = Active
	*events = append(*events, event(s, GrantAcknowledged, now, c.Actor, s.Lease.WaiterID, s.Lease.ID, "agent installed generation"))
	if higherWaiting(*s) {
		s.Phase = YieldRequested
		s.Lease.YieldRequestedAt = now
		*events = append(*events, event(s, YieldAsked, now, c.Actor, "", s.Lease.ID, "higher-priority waiter"))
	}
	return nil
}

// admitYield is the sole admission rule for asking a board to yield: there is
// a holder that can still be asked, and the named waiter is queued and
// outranks it. PlanYield shares it with requestYield on purpose, so a plan can
// never quote an ETA for a yield Apply would refuse.
func admitYield(s Snapshot, waiterID string) error {
	if waiterID == "" {
		return &Error{InvalidArgument, "missing actor or waiter ID"}
	}
	if s.Lease == nil || (s.Phase != Active && s.Phase != YieldRequested && s.Phase != Draining) {
		return &Error{Conflict, "no active holder to ask"}
	}
	for _, w := range s.Queue {
		if w.ID == waiterID && w.Class > s.Lease.Class {
			return nil
		}
	}
	return &Error{Denied, "named waiter does not outrank holder"}
}

func requestYield(s *Snapshot, c RequestYield, now time.Time, events *[]Event) error {
	if c.Actor == "" {
		return &Error{InvalidArgument, "missing actor or waiter ID"}
	}
	if c.ShownTarget < 0 || c.ShownTarget > MaxHandoffBound {
		return &Error{InvalidArgument, "shown handoff target is out of range"}
	}
	// A target and its cohort travel together or not at all. A number with
	// no comparable history named behind it is unfalsifiable: nothing can
	// later say which work it described, so nothing can say it was missed.
	if c.Cohort != (YieldCohort{}) {
		if err := ValidateYieldCohort(c.Cohort); err != nil {
			return err
		}
	} else if c.ShownTarget > 0 {
		return &Error{InvalidArgument, "shown handoff target has no cohort behind it"}
	}
	if err := admitYield(*s, c.WaiterID); err != nil {
		return err
	}
	if s.Phase == Active {
		s.Phase = YieldRequested
		s.Lease.YieldRequestedAt = now
		// Only the transition records the target. A repeat request against a
		// board already asked is a no-op here, and it must stay one: the
		// first requester's ETA is the promise, and letting a later caller
		// overwrite it is how a deadline slides without anyone deciding to
		// move it.
		s.Lease.HandoffTarget = c.ShownTarget
		s.Lease.HandoffCohort = c.Cohort
		*events = append(*events, event(s, YieldAsked, now, c.Actor, c.WaiterID, s.Lease.ID, "higher-priority waiter"))
	}
	return nil
}

func beginDrain(s *Snapshot, c BeginDrain, now time.Time, events *[]Event) error {
	if c.Actor == "" {
		return &Error{InvalidArgument, "missing holder identity"}
	}
	if err := current(s, c.LeaseID, c.Generation, now); err != nil {
		return err
	}
	if s.Phase != YieldRequested {
		return &Error{Conflict, "yield was not requested"}
	}
	s.Phase = Draining
	*events = append(*events, event(s, DrainStarted, now, c.Actor, s.Lease.WaiterID, s.Lease.ID, "safe checkpoint reached"))
	return nil
}

func release(s *Snapshot, c Release, now time.Time, events *[]Event) error {
	if c.Actor == "" {
		return &Error{InvalidArgument, "missing holder identity"}
	}
	if err := current(s, c.LeaseID, c.Generation, now); err != nil {
		return err
	}
	if s.Phase != Active && s.Phase != YieldRequested && s.Phase != Draining {
		return &Error{Conflict, "lease is not active"}
	}
	if c.NeutralReceipt == "" {
		s.Phase = RecoveryRequired
		*events = append(*events, event(s, RecoveryNeeded, now, c.Actor, s.Lease.WaiterID, s.Lease.ID, "release lacked neutral receipt"))
		return nil
	}
	old := s.Lease
	s.Lease = nil
	s.Phase = Ready
	*events = append(*events, event(s, LeaseReleased, now, c.Actor, old.WaiterID, old.ID, c.NeutralReceipt))
	return grantNext(s, now, c.Actor, events)
}

func extend(s *Snapshot, c Extend, now time.Time, events *[]Event) error {
	if c.Actor == "" {
		return &Error{InvalidArgument, "missing holder identity"}
	}
	if err := current(s, c.LeaseID, c.Generation, now); err != nil {
		return err
	}
	if s.Phase != Active && s.Phase != YieldRequested && s.Phase != Draining {
		return &Error{Conflict, "lease is not active"}
	}
	if c.Reason == "" || c.NewExpiry.IsZero() || !c.NewExpiry.After(s.Lease.ExpiresAt) {
		return &Error{InvalidArgument, "extension needs reason and later expiry"}
	}
	if c.NewExpiry.After(s.Lease.GrantedAt.Add(classCeiling(s.Lease.Class))) {
		return &Error{Denied, "class lifetime ceiling exceeded"}
	}
	additional := c.NewExpiry.Sub(s.Lease.ExpiresAt)
	if higherWaiting(*s) || sameClassHumanWaiting(*s) {
		if additional > 10*time.Minute-s.Lease.ContendedExtensionUsed {
			return &Error{Denied, "contended safe-wrap-up extension exceeds ten minutes"}
		}
		s.Lease.ContendedExtensionUsed += additional
	}
	if s.Lease.DeadlineVersion == math.MaxUint64 {
		return &Error{Conflict, "deadline version exhausted"}
	}
	s.Lease.ExpiresAt = c.NewExpiry
	s.Lease.DeadlineVersion++
	*events = append(*events, event(s, LeaseExtended, now, c.Actor, s.Lease.WaiterID, s.Lease.ID, c.Reason))
	return nil
}

func agentUnavailable(s *Snapshot, c AgentUnavailable, now time.Time, events *[]Event) error {
	if c.Actor == "" || c.Reason == "" {
		return &Error{InvalidArgument, "missing actor or reason"}
	}
	if s.Phase == Recovering {
		return quarantine(s, c.Actor, "agent unavailable during recovery: "+c.Reason, now, events)
	}
	if s.Phase == Quarantined || s.Phase == RecoveryRequired {
		return nil
	}
	s.Phase = RecoveryRequired
	*events = append(*events, event(s, RecoveryNeeded, now, c.Actor, "", leaseID(s), c.Reason))
	return nil
}

func beginRecovery(s *Snapshot, c BeginRecovery, now time.Time, events *[]Event) error {
	if c.Actor == "" || c.PlanID == "" || c.Reason == "" {
		return &Error{InvalidArgument, "recovery requires actor, plan, and reason"}
	}
	if s.Phase != RecoveryRequired && s.Phase != Quarantined {
		return &Error{Conflict, "board is not awaiting recovery"}
	}
	s.Phase = Recovering
	*events = append(*events, event(s, RecoveryStarted, now, c.Actor, "", leaseID(s), c.PlanID+": "+c.Reason))
	return nil
}

func completeRecovery(s *Snapshot, c CompleteRecovery, now time.Time, events *[]Event) error {
	if c.Actor == "" || c.NeutralReceipt == "" {
		return &Error{InvalidArgument, "recovery requires actor and neutral receipt"}
	}
	if s.Phase != Recovering {
		return &Error{Conflict, "recovery is not in progress"}
	}
	if c.AgentHighWater < s.AgentHighWater {
		return quarantine(s, c.Actor, "agent high-water regressed during recovery", now, events)
	}
	if c.AgentHighWater > s.Generation {
		s.Generation = c.AgentHighWater
		*events = append(*events, event(s, GenerationReconciled, now, c.Actor, "", leaseID(s), "operator recovery advanced server generation"))
	}
	s.AgentHighWater = c.AgentHighWater
	s.Lease = nil
	s.Phase = Ready
	*events = append(*events, event(s, RecoveryFinished, now, c.Actor, "", "", c.NeutralReceipt))
	return grantNext(s, now, c.Actor, events)
}

func quarantine(s *Snapshot, actor, reason string, now time.Time, events *[]Event) error {
	if actor == "" || reason == "" {
		return &Error{InvalidArgument, "quarantine requires actor and reason"}
	}
	if s.Phase == Quarantined {
		return nil
	}
	s.Phase = Quarantined
	*events = append(*events, event(s, BoardQuarantined, now, actor, "", leaseID(s), reason))
	return nil
}

func observeGeneration(s *Snapshot, c ObserveAgentGeneration, now time.Time, events *[]Event) error {
	if c.Actor == "" {
		return &Error{InvalidArgument, "missing agent identity"}
	}
	if c.HighWater > s.Generation || c.HighWater < s.AgentHighWater {
		if c.HighWater > s.AgentHighWater {
			s.AgentHighWater = c.HighWater
		}
		return quarantine(s, c.Actor, "agent high-water contradicts database", now, events)
	}
	if c.HighWater != s.AgentHighWater {
		s.AgentHighWater = c.HighWater
		*events = append(*events, event(s, AgentObserved, now, c.Actor, "", leaseID(s), "high-water advanced"))
	}
	return nil
}

func expire(s *Snapshot, now time.Time, events *[]Event) bool {
	if s.Lease == nil || now.Before(s.Lease.ExpiresAt) {
		return false
	}
	switch s.Phase {
	case GrantPending, Active, YieldRequested, Draining:
		s.Phase = RecoveryRequired
		*events = append(*events, event(s, LeaseExpired, now, "server", s.Lease.WaiterID, s.Lease.ID, "deadline reached"))
		return true
	default:
		return false
	}
}

func grantNext(s *Snapshot, now time.Time, actor string, events *[]Event) error {
	if s.Phase != Ready || s.Lease != nil || len(s.Queue) == 0 {
		return nil
	}
	if s.AgentHighWater > s.Generation {
		return &Error{RecoveryNecessary, "agent generation exceeds database"}
	}
	if s.Generation == math.MaxUint64 {
		return &Error{Conflict, "lease generation exhausted"}
	}
	best := 0
	for i := 1; i < len(s.Queue); i++ {
		if s.Queue[i].Class > s.Queue[best].Class || (s.Queue[i].Class == s.Queue[best].Class && s.Queue[i].Sequence < s.Queue[best].Sequence) {
			best = i
		}
	}
	w := s.Queue[best]
	s.Queue = append(s.Queue[:best], s.Queue[best+1:]...)
	s.Generation++
	s.Lease = &Lease{ID: w.LeaseID, WaiterID: w.ID, Holder: w.Holder, Class: w.Class, Reason: w.Reason, Generation: s.Generation, GrantedAt: now, ExpiresAt: now.Add(w.Duration), RequestedDuration: w.Duration, DeadlineVersion: 1}
	s.Phase = GrantPending
	*events = append(*events, event(s, GrantCreated, now, actor, w.ID, w.LeaseID, w.Reason))
	return nil
}

func current(s *Snapshot, leaseID string, generation uint64, now time.Time) error {
	if s.Lease == nil || leaseID == "" || generation == 0 {
		return &Error{StaleGeneration, "no matching grant"}
	}
	if leaseID != s.Lease.ID || generation != s.Lease.Generation {
		return &Error{StaleGeneration, "lease token is stale"}
	}
	if !now.Before(s.Lease.ExpiresAt) {
		return &Error{Expired, "lease has expired"}
	}
	return nil
}

func higherWaiting(s Snapshot) bool {
	if s.Lease == nil {
		return false
	}
	for _, w := range s.Queue {
		if w.Class > s.Lease.Class {
			return true
		}
	}
	return false
}

func sameClassHumanWaiting(s Snapshot) bool {
	if s.Lease == nil || s.Lease.Class != ClassHuman {
		return false
	}
	for _, w := range s.Queue {
		if w.Class == ClassHuman {
			return true
		}
	}
	return false
}

func leaseID(s *Snapshot) string {
	if s.Lease == nil {
		return ""
	}
	return s.Lease.ID
}

// MaxClockOffset is the largest acceptable measured worst-case UTC offset.
const MaxClockOffset = 2 * time.Second

// ClockSafetyMargin is subtracted in addition to the measured offset.
const ClockSafetyMargin = 2 * time.Second

// DeadlineFence is an in-process monotonic deadline for one installed grant.
// It is not durable: a process restart must obtain a fresh authenticated grant
// and clock measurement, and cannot restore authority from wall time alone.
type DeadlineFence struct {
	Generation uint64
	Version    uint64
	Until      time.Time
}

// SeedDeadline converts a server UTC expiry to a conservative local monotonic
// deadline at receipt. localNow must come from time.Now on the agent and retain
// its monotonic component; its UTC offset bound must have been measured.
func SeedDeadline(generation, version uint64, expiryUTC, localNow time.Time, offsetBound time.Duration) (DeadlineFence, error) {
	until, err := calculateDeadline(expiryUTC, localNow, offsetBound)
	if err != nil {
		return DeadlineFence{}, err
	}
	if generation == 0 || version == 0 {
		return DeadlineFence{}, &Error{InvalidArgument, "zero generation or deadline version"}
	}
	return DeadlineFence{Generation: generation, Version: version, Until: until}, nil
}

// RefreshDeadline can only shorten a heartbeat deadline. A separately audited
// higher-version extension may lengthen it after a fresh offset measurement.
func RefreshDeadline(previous DeadlineFence, generation, version uint64, expiryUTC, localNow time.Time, offsetBound time.Duration, authorizedExtension bool) (DeadlineFence, error) {
	if generation != previous.Generation || version < previous.Version || previous.Generation == 0 {
		return DeadlineFence{}, &Error{StaleGeneration, "deadline fence generation or version is stale"}
	}
	if localNow.IsZero() || !localNow.Before(previous.Until) {
		return DeadlineFence{}, &Error{Expired, "local monotonic deadline has passed"}
	}
	if authorizedExtension && version <= previous.Version {
		return DeadlineFence{}, &Error{Conflict, "extension requires a higher deadline version"}
	}
	until, err := calculateDeadline(expiryUTC, localNow, offsetBound)
	if err != nil {
		return DeadlineFence{}, err
	}
	if !authorizedExtension && until.After(previous.Until) {
		until = previous.Until
	}
	return DeadlineFence{Generation: generation, Version: version, Until: until}, nil
}

// CanStartSegment requires enough local monotonic authority for a complete
// indivisible segment plus its declared recovery margin. The server-side
// snapshot check is also required; neither check substitutes for the other.
func (f DeadlineFence) CanStartSegment(generation uint64, localNow time.Time, bound, recoveryMargin time.Duration) error {
	if generation == 0 || localNow.IsZero() || bound <= 0 || recoveryMargin < 0 {
		return &Error{InvalidArgument, "invalid generation, time, bound, or margin"}
	}
	if f.Generation != generation || f.Until.IsZero() {
		return &Error{StaleGeneration, "local deadline fence is absent or stale"}
	}
	if !localNow.Before(f.Until) {
		return &Error{Expired, "local monotonic deadline has passed"}
	}
	if bound > f.Until.Sub(localNow)-recoveryMargin {
		return &Error{Deadline, "insufficient monotonic lease time for segment and recovery"}
	}
	return nil
}

func calculateDeadline(expiryUTC, localNow time.Time, offsetBound time.Duration) (time.Time, error) {
	if expiryUTC.IsZero() || localNow.IsZero() || offsetBound < 0 || offsetBound > MaxClockOffset {
		return time.Time{}, &Error{InvalidArgument, "invalid or unbounded UTC offset"}
	}
	remaining := expiryUTC.Sub(localNow) - offsetBound - ClockSafetyMargin
	if remaining <= 0 {
		return time.Time{}, &Error{Expired, "conservative local deadline has passed"}
	}
	return localNow.Add(remaining), nil
}

// Validate checks structural invariants before a snapshot is loaded or stored.
func Validate(s Snapshot) error {
	if s.BoardID == "" {
		return &Error{InvalidArgument, "empty board ID"}
	}
	if len(s.Queue) > MaxWaiters {
		return &Error{Conflict, "board wait queue exceeds maximum"}
	}
	if s.AgentHighWater > s.Generation && s.Phase != Quarantined && s.Phase != Recovering {
		return &Error{Conflict, "agent high-water exceeds server generation outside quarantine"}
	}
	if s.Lease != nil {
		lease := s.Lease
		if lease.ID == "" || lease.WaiterID == "" || lease.Holder == "" || lease.Reason == "" || lease.Generation == 0 || !validClass(lease.Class) || lease.GrantedAt.IsZero() || lease.RequestedDuration <= 0 || lease.RequestedDuration > classCeiling(lease.Class) || lease.DeadlineVersion == 0 || !lease.ExpiresAt.After(lease.GrantedAt) || lease.ExpiresAt.After(lease.GrantedAt.Add(classCeiling(lease.Class))) || lease.ContendedExtensionUsed < 0 || lease.ContendedExtensionUsed > 10*time.Minute {
			return &Error{Conflict, "invalid retained lease"}
		}
		if lease.Generation > s.Generation {
			return &Error{Conflict, "lease generation exceeds board generation"}
		}
		// A beat stamped before the grant it belongs to is evidence of a
		// replayed or misattributed report, not of a live holder.
		if !lease.LastHeartbeatAt.IsZero() && lease.LastHeartbeatAt.Before(lease.GrantedAt) {
			return &Error{Conflict, "retained lease carries a heartbeat from before its grant"}
		}
		if lease.HandoffTarget < 0 || lease.HandoffTarget > MaxHandoffBound {
			return &Error{Conflict, "retained lease carries an out-of-range handoff target"}
		}
		// A target with no request behind it is a promise nobody made. It
		// would read as an outstanding deadline to anything measuring the
		// handoff, so it is refused rather than ignored.
		if lease.HandoffTarget != 0 && lease.YieldRequestedAt.IsZero() {
			return &Error{Conflict, "retained lease carries a handoff target without a yield request"}
		}
		if lease.HandoffCohort != (YieldCohort{}) {
			if lease.YieldRequestedAt.IsZero() {
				return &Error{Conflict, "retained lease carries a handoff cohort without a yield request"}
			}
			if err := ValidateYieldCohort(lease.HandoffCohort); err != nil {
				return &Error{Conflict, "retained lease carries an invalid handoff cohort"}
			}
		} else if lease.HandoffTarget != 0 {
			return &Error{Conflict, "retained lease carries a handoff target without its cohort"}
		}
	}
	switch s.Phase {
	case Ready:
		if s.Lease != nil {
			return &Error{Conflict, "ready board retains lease"}
		}
	case GrantPending, Active, YieldRequested, Draining:
		if s.Lease == nil || s.Lease.Generation != s.Generation {
			return &Error{Conflict, "live phase lacks valid current lease"}
		}
		if s.Phase != GrantPending && s.AgentHighWater != s.Generation {
			return &Error{Conflict, "active grant has not been installed by agent"}
		}
		if (s.Phase == YieldRequested || s.Phase == Draining) && s.Lease.YieldRequestedAt.IsZero() {
			return &Error{Conflict, "yield phase lacks request time"}
		}
	case RecoveryRequired, Recovering, Quarantined:
		// The old lease is retained as recovery evidence, if one existed.
	default:
		return &Error{InvalidArgument, fmt.Sprintf("unknown board phase %q", s.Phase)}
	}
	ids := make(map[string]struct{}, len(s.Queue))
	leaseIDs := make(map[string]struct{}, len(s.Queue))
	sequences := make(map[uint64]struct{}, len(s.Queue))
	for _, w := range s.Queue {
		if w.ID == "" || w.LeaseID == "" || w.Holder == "" || w.Reason == "" || !validClass(w.Class) || w.Duration <= 0 || w.Duration > classCeiling(w.Class) || w.QueuedAt.IsZero() || w.Sequence == 0 || w.Sequence > s.NextSequence {
			return &Error{Conflict, "invalid waiter"}
		}
		if s.Lease != nil && (w.ID == s.Lease.WaiterID || w.LeaseID == s.Lease.ID) {
			return &Error{Conflict, "queued waiter collides with retained lease"}
		}
		if _, found := ids[w.ID]; found {
			return &Error{Conflict, "duplicate waiter ID"}
		}
		if _, found := leaseIDs[w.LeaseID]; found {
			return &Error{Conflict, "duplicate queued lease ID"}
		}
		if _, found := sequences[w.Sequence]; found {
			return &Error{Conflict, "duplicate queue sequence"}
		}
		ids[w.ID] = struct{}{}
		leaseIDs[w.LeaseID] = struct{}{}
		sequences[w.Sequence] = struct{}{}
	}
	return nil
}
