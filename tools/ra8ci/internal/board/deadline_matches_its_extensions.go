package board

import "time"

// checkDeadlineMatchesItsExtensions holds a retained lease's expiry to the
// grant and the extensions that are actually on record.
//
// The reducer writes ExpiresAt in exactly two places. grantNext sets it to
// GrantedAt plus the duration the waiter asked for and stamps DeadlineVersion
// 1 in the same literal. extend is the only other writer: it demands a
// strictly later expiry, refuses one past the class lifetime ceiling, and
// increments DeadlineVersion in the same step as it moves the deadline, so
// every move of ExpiresAt costs exactly one version and leaves a LeaseExtended
// event behind it. Nothing anywhere shortens a deadline.
//
// Two retained shapes therefore cannot have come from here, and each is
// damaging in its own direction.
//
// An expiry EARLIER than the grant plus the requested duration is a deadline
// that moved backwards. Every deadline reader treats ExpiresAt as authority
// the holder already has: expire() sends the board into recovery the moment
// now reaches it, current() refuses the holder's own Release and BeginDrain
// past it, and both CanStartSegment checks measure the remaining segment
// budget from it. A shortened expiry cuts a holder off mid-run against a
// deadline nobody issued, and the board goes to recovery carrying a lease that
// looks like an ordinary timeout.
//
// An expiry LATER than the grant plus the requested duration while
// DeadlineVersion is still 1 is the opposite: time the holder was never
// granted and no extension paid for. The existing ceiling check bounds it only
// at the class lifetime, so an AI lease asked for ten minutes can sit at
// fifty-five without tripping anything, and DeadlineVersion 1 says no
// LeaseExtended event was ever emitted, which is precisely the audit record an
// operator would read to find out who lengthened it. The contended-extension
// accounting is blind to it too: ContendedExtensionUsed is only charged inside
// extend, so a deadline that never went through extend also never spent the
// ten-minute contended budget it should have.
//
// The rule is deliberately silent about HOW MUCH later a lease at version 2 or
// beyond sits: the extensions are individually bounded and separately
// audited, and this door only asks whether the distance from the grant is
// accounted for at all.
func checkDeadlineMatchesItsExtensions(lease *Lease) error {
	if lease == nil {
		return nil
	}
	granted := grantedExpiry(*lease)
	if lease.ExpiresAt.Before(granted) {
		return &Error{Conflict, "retained lease expires before the duration its grant issued"}
	}
	if lease.ExpiresAt.After(granted) && lease.DeadlineVersion == 1 {
		return &Error{Conflict, "retained lease outlives its grant with no extension on record"}
	}
	return nil
}

// grantedExpiry is where grantNext put this lease's deadline: the moment the
// grant was made plus the duration the waiter asked for. Stated once so the
// rule and its tests cannot drift apart.
func grantedExpiry(lease Lease) time.Time {
	return lease.GrantedAt.Add(lease.RequestedDuration)
}
