package neutral

import "time"

// checkObservationIsFresh holds a signed receipt to the same observation
// freshness the producer signs under: the gap from the physical observation to
// the signature is at most maxObservationAge.
//
// The producer already refuses a stale observation (validObservation:
// now.Sub(o.ObservedAt) <= maxObservationAge, where that now is the stamp it
// writes as SignedAt). The verifier held ObservedAt to the challenge window
// alone, refusing only a stamp before IssuedAt or after SignedAt. A challenge
// may live up to maxChallengeAge, so a receipt could be verified whose
// observation was made six times further from its signature than any producer
// here would sign, and the verifier is the side that decides.
//
// The age rule is not about the receipt going stale, it is about what the
// observation still proves. It is a reading of a physical fixture: power
// isolated, SWD idle. The board is released or recovered on the strength of
// that reading, and what the reading says about the fixture now decays with
// every second between looking and swearing to it. A one-use challenge that
// is still live says the challenge has not been spent, not that nothing has
// touched the board since it was looked at.
func checkObservationIsFresh(observedAt, signedAt time.Time) bool {
	return !observedAt.After(signedAt) && signedAt.Sub(observedAt) <= maxObservationAge
}
