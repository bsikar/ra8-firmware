// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"errors"
	"fmt"
	"time"
)

// Drain evidence is judged at the door and spent at the mutation.
//
// Stop is a hard VM stop and Destroy deletes the guest, so neither may assume
// the guest is idle; both require a durable drain observation, and that
// observation is only accepted while it is fresh. The window is short on
// purpose. GitHub can assign a job to a runner at any moment, so a drain
// observed long enough ago is not weak evidence that the guest is idle, it is
// no evidence at all.
//
// The window was enforced once, at the top of Stop and Destroy, and the
// operation it authorises is issued much later. Between the two, both methods
// call Get, which is three round trips (the cluster listing, the guest
// configuration, the guest status), each bounded by the request timeout rather
// than by anything the freshness rule knows about. A client configured to the
// policy ceiling can spend a minute and a half in those reads and still be
// inside its operation timeout, and the stop then went out on a drain
// observation this package had already decided was good for ten seconds.
//
// That is the ordinary shape of a slow API day, not an exotic one, and the
// cost lands exactly where the window exists to prevent it: a hard stop or a
// delete issued against a guest that picked up a job while this client was
// reading about it.
//
// So the window is re-judged immediately before the request goes out. A proof
// that aged in between is not a caller mistake and nothing about the guest is
// wrong; observing the drain again is the whole fix, which is why it is
// reported as its own fact rather than folded into the refusal at the door.

// ErrEvidenceStale is drain and idle evidence that was fresh when this client
// accepted it and had aged past the window by the time the operation was about
// to be issued. It wraps ErrInvalid as well, so a caller that only asks
// whether the input was refused is unaffected.
var ErrEvidenceStale = errors.New("Proxmox idle evidence went stale before the operation was issued")

const (
	// idleProofFreshness is how long a durable drain observation speaks for
	// the guest.
	idleProofFreshness = 10 * time.Second
	// idleProofSkew tolerates a small clock difference between whatever
	// recorded the observation and this process.
	idleProofSkew = time.Second
)

// idleProofIsFresh is the one freshness rule, applied at the door by
// validateIdleProof and again at the mutation by checkIdleProofStillFresh.
// Both callers ask this function rather than restating the bound, so the two
// checks cannot drift into two different windows.
//
// An unobserved proof is never fresh, an observation from the future is
// refused past the skew allowance rather than read as very recent, and the
// bound itself is inclusive: a proof exactly at the window is still accepted,
// as it always was.
func idleProofIsFresh(proof IdleProof, now time.Time) bool {
	if proof.ObservedAt.IsZero() || proof.ObservedAt.After(now.Add(idleProofSkew)) {
		return false
	}
	return now.Sub(proof.ObservedAt) <= idleProofFreshness
}

// checkIdleProofStillFresh re-judges the clock half of a proof that has
// already been accepted, and only the clock half: every other field is a
// property of the value the caller handed over, which cannot have changed
// since, so re-reading it would say nothing new. Time is the only thing that
// moved.
func checkIdleProofStillFresh(proof IdleProof, now time.Time) error {
	if idleProofIsFresh(proof, now) {
		return nil
	}
	return fmt.Errorf("%w: %w: drain and idle evidence for VM %d was observed %s ago, past the %s this client acts on",
		ErrInvalid, ErrEvidenceStale, proof.VMID,
		now.Sub(proof.ObservedAt).Truncate(time.Millisecond), idleProofFreshness)
}
