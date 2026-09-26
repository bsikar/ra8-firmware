package board

import "time"

// MaxHeartbeatInterval bounds the reporting interval a caller may observe
// liveness against. Three missed beats at the ceiling is thirty minutes, which
// still fits inside the shortest class lifetime ceiling (an AI lease may run
// for one hour), so a holder can be reported overdue before its authority ends
// rather than only after expiry has already answered the question.
const MaxHeartbeatInterval = 10 * time.Minute

// HeartbeatGraceBeats is how many consecutive reporting intervals may pass in
// silence before a holder is reported overdue. One lost beat is a dropped
// packet rather than a crash, and reporting on the first miss would make a
// routine network hiccup look like a holder that needs recovering.
const HeartbeatGraceBeats = 3

// HolderHeartbeat records that the current holder is still alive. It is the
// liveness half of the lease invariant that every lease carries a requested
// duration, absolute expiry, heartbeat, holder identity, reason, and
// generation token.
//
// It is deliberately not a deadline command in either direction. A heartbeat
// cannot lengthen a lease: that is Extend, which demands a reason and is held
// to a policy limit. A missed heartbeat cannot shorten one either: on holder
// crash the server waits for expiry and then asks the board-side agent for a
// reviewed recovery sequence, so silence is evidence to report, never
// authority to withdraw.
type HolderHeartbeat struct {
	Actor      string
	LeaseID    string
	Generation uint64
}

func (HolderHeartbeat) boardCommand() {}

// HolderLiveness is what the board can say about its holder still being there.
// It is a report, not a transition: deciding what silence warrants (waiting for
// expiry, declaring the agent unavailable, calling an operator) belongs to the
// caller that knows the deployment, not to the state machine.
type HolderLiveness struct {
	// Held reports whether a holder exists to have an opinion about. A free,
	// recovering, or quarantined board is not held and every other field is
	// zero.
	Held bool

	LeaseID string
	Holder  string
	Class   Class

	// LastSeenAt is the most recent beat, or the grant itself when no beat
	// has arrived yet: being granted the board is itself an observation
	// that the holder was there.
	LastSeenAt time.Time

	// Beat reports whether LastSeenAt came from a heartbeat rather than
	// from the grant.
	Beat bool

	Silence  time.Duration
	Interval time.Duration

	// Overdue reports that the silence has passed the grace. It is never a
	// statement that the lease has ended.
	Overdue bool

	// ExpiresAt is when the holder's authority actually ends, which is the
	// only clock that does end it.
	ExpiresAt time.Time
}

// Explain states the liveness in one line for an operator or a waiter.
func (l HolderLiveness) Explain() string {
	if !l.Held {
		return "board is not held"
	}
	if l.Overdue {
		return "holder " + l.Holder + " has not reported for " + l.Silence.String() + "; authority still runs to expiry"
	}
	return "holder " + l.Holder + " last reported " + l.Silence.String() + " ago"
}

// ObserveHolderLiveness reports how long the current holder has been silent,
// measured against the interval it is expected to report on. It never mutates
// the snapshot and never ends a lease.
func ObserveHolderLiveness(s Snapshot, now time.Time, interval time.Duration) (HolderLiveness, error) {
	if now.IsZero() {
		return HolderLiveness{}, &Error{InvalidArgument, "missing observation time"}
	}
	if interval <= 0 || interval > MaxHeartbeatInterval {
		return HolderLiveness{}, &Error{InvalidArgument, "heartbeat interval out of range"}
	}
	if err := Validate(s); err != nil {
		return HolderLiveness{}, err
	}
	if s.Lease == nil || !liveHolderPhase(s.Phase) {
		return HolderLiveness{}, nil
	}
	lease := s.Lease
	seen, beat := lease.GrantedAt, false
	if !lease.LastHeartbeatAt.IsZero() {
		seen, beat = lease.LastHeartbeatAt, true
	}
	silence := now.Sub(seen)
	if silence < 0 {
		silence = 0
	}
	return HolderLiveness{
		Held:       true,
		LeaseID:    lease.ID,
		Holder:     lease.Holder,
		Class:      lease.Class,
		LastSeenAt: seen,
		Beat:       beat,
		Silence:    silence,
		Interval:   interval,
		Overdue:    silence > time.Duration(HeartbeatGraceBeats)*interval,
		ExpiresAt:  lease.ExpiresAt,
	}, nil
}

func liveHolderPhase(phase Phase) bool {
	return phase == GrantPending || phase == Active || phase == YieldRequested || phase == Draining
}

// holderHeartbeat records the beat and reports whether the snapshot changed.
// It emits no event: the audited set is take, grant, extend, yield request,
// checkpoint, release, expiry, recovery, and denied action, and a record every
// few seconds per board would bury all of them. A refused beat is audited by
// Apply as a denied action, which is the case worth reading: it is a holder
// acting after its generation was superseded.
func holderHeartbeat(s *Snapshot, c HolderHeartbeat, now time.Time) (bool, error) {
	if c.Actor == "" {
		return false, &Error{InvalidArgument, "missing holder identity"}
	}
	if err := current(s, c.LeaseID, c.Generation, now); err != nil {
		return false, err
	}
	if !liveHolderPhase(s.Phase) {
		return false, &Error{Conflict, "board is not held"}
	}
	// A beat delivered out of order must not make the holder look less
	// recently seen than it already is, so the later observation stands.
	// The grant counts as an observation too, which is what makes a beat
	// from before it change nothing.
	if !beatAddsAnObservation(s.Lease, now) {
		return false, nil
	}
	s.Lease.LastHeartbeatAt = now
	return true, nil
}

// EventFreeCommand reports whether a command may advance the snapshot version
// without emitting a board event. HolderHeartbeat is the only one, for the
// reason holderHeartbeat states: the audited set is take, grant, extend, yield
// request, checkpoint, release, expiry, recovery, and denied action, and a
// record every few seconds per board would bury all of them.
//
// It exists so the store can tell that case apart from a reducer bug. Every
// other command that moves the version owes an event, and a version that moves
// with neither an event nor this predicate is a defect the store must refuse
// rather than commit.
func EventFreeCommand(command Command) bool {
	_, beat := command.(HolderHeartbeat)
	return beat
}
