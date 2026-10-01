package board

import "time"

// checkContendedBudgetIsAccountedFor holds a retained lease's spent contended
// budget to the extensions that paid for it.
//
// ContendedExtensionUsed is the one lease field that refuses a holder
// something. extend charges it only when a higher-priority waiter or a
// same-class human is already queued, and the next contended extension is
// denied the moment the additional time would carry the total past ten
// minutes. Validate bounds the field at that ceiling and at zero, and nothing
// asked where the charge came from.
//
// It can only have come from one place. extend charges exactly the time it
// adds, moves ExpiresAt by that same span, and increments DeadlineVersion in
// the same step, so every second on this counter is a second the deadline
// visibly moved and a LeaseExtended event an operator can read. Two retained
// shapes therefore cannot have been written here.
//
// A charge with DeadlineVersion still 1 is budget spent by a lease that never
// extended at all: no deadline moved, no event was emitted, and there is
// nothing in the audit trail to explain the number. checkDeadlineMatchesItsExtensions
// refuses the mirror image of this, a deadline that moved with no extension on
// record; this is the same accident seen from the counter.
//
// A charge larger than the whole distance from the granted expiry is budget
// spent beyond every second the deadline ever moved, at any version. The
// contended charges are a subset of the total time added, since an uncontended
// extension moves the deadline and charges nothing, so the distance from the
// grant is the ceiling the counter can never pass.
//
// Both run one way, and it is the expensive way: an overstated charge denies
// the holder a safe-wrap-up extension it never spent. That refusal lands
// exactly when the board is contended, which is when a holder most needs the
// few minutes to park hardware cleanly, and the holder is instead cut off at an
// expiry it could have moved. The understated direction is not visible from a
// snapshot and is not guessed at here.
//
// A lease at version 2 or beyond whose charge fits inside the extended
// distance is accepted without further argument: the individual extensions are
// separately bounded and audited, and this door only asks whether the charge
// is accounted for at all.
func checkContendedBudgetIsAccountedFor(lease *Lease) error {
	if lease == nil || lease.ContendedExtensionUsed <= 0 {
		return nil
	}
	if lease.DeadlineVersion == 1 {
		return &Error{Conflict, "retained lease spent contended budget with no extension on record"}
	}
	if lease.ContendedExtensionUsed > extendedDistance(*lease) {
		return &Error{Conflict, "retained lease spent more contended budget than its deadline ever moved"}
	}
	return nil
}

// extendedDistance is how far the deadline has moved past the grant, which is
// the sum of every extension this lease was given. Stated once so the rule and
// its tests cannot drift apart.
func extendedDistance(lease Lease) time.Duration {
	return lease.ExpiresAt.Sub(grantedExpiry(lease))
}
