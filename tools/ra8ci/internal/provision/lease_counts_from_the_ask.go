// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"errors"
	"time"
)

// leaseStartsWhenVaultWasAsked answers the one stamp the remaining-lease
// arithmetic is built on: the moment from which a token's lease is counted.
//
// Vault states a lease as a DURATION, never as an expiry, so the plane has to
// supply the origin itself and everything downstream is measured from it.
// checkTokenLeaseCoversOperation subtracts it (issued.Add(lease).Sub(now)) to
// decide whether a token covers one Terraform command, and that subtraction is
// only as honest as this stamp.
//
// The stamp was taken AFTER the login round trip returned, which is the one
// moment it cannot be. The lease starts when Vault mints the token, which
// happens inside the request, before the answer is written, sent back across
// the network and read here. Everything in between is lease the plane has
// already spent and does not know it spent: the TLS handshake, Vault's own
// work, the return trip, the body read. None of that is hypothetical on this
// path. The AppRole timeout may be configured as high as 30s, the accepted
// lease starts at 1s, and a Vault under load answers slowly exactly when the
// rest of the plane is busy too.
//
// The error runs one way only, and it is the wrong way. Stamping late makes
// the token look YOUNGER than it is, so the remaining lease is overstated by
// the whole round trip and checkTokenLeaseCoversOperation approves a token that
// it would refuse if it knew. That check exists precisely to keep a credential
// from dying inside a running Terraform command, where run() cannot tell what
// the child hit and reports "Terraform command failed; reconcile state before
// retry" over a consumed apply intent and half-written remote state.
//
// So the lease is counted from the moment the request went out, which is the
// earliest moment the token could exist. That is the fail-closed direction:
// the plane now understates the remaining lease by the round trip rather than
// overstating it, and understating costs at most one refused session on a
// credential that was nearly spent anyway.
//
// A round trip that appears to end before it began is refused rather than
// papered over. Both stamps come from time.Now() in this process and carry
// monotonic readings, so the pair cannot invert on an ordinary host; a pair
// that does is a stamp this function was handed rather than one it can reason
// about, and a lease origin nobody can defend is not one to hand Terraform.
func leaseStartsWhenVaultWasAsked(askedAt, answeredAt time.Time) (time.Time, error) {
	if askedAt.IsZero() || answeredAt.IsZero() {
		return time.Time{}, errors.New("Vault login carries no stamp to count its lease from")
	}
	if answeredAt.Before(askedAt) {
		return time.Time{}, errors.New("Vault login answered before it was asked")
	}
	return askedAt, nil
}
