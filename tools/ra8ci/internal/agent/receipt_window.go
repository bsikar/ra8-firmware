// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import "time"

// logFlushWindow bounds the one kept-back log chunk's last offer, on a budget
// of its own rather than the terminal receipt's.
//
// The finishing phase posts two things and they are not worth the same. The
// held chunk is best-effort evidence the receipt itself already reports on:
// when it does not land, the receipt carries log_upload_error, states a final
// sequence at the last chunk that did land, and clears EvidenceComplete, so a
// reader is told exactly what is missing. The terminal receipt is the only
// word the plane ever gets about how this attempt ended, and nothing
// reconstructs it: without it the attempt sits under its fence until the lease
// expires and an operator reads a runner that went silent.
//
// Both used to travel on one context bounded by requestLimit. A plane that
// accepted the connection and then stalled on the logs endpoint spent that
// whole window on the optional half; the receipt was then posted on an already
// expired context, failed immediately, and the attempt reported nothing at all.
// The artifact path was given a separate window for precisely this reason and
// says so in artifact_attempt.go; the log flush was left sharing.
//
// Three seconds is the whole of this last offer. It is one already-built chunk
// on a connection this agent has been using all attempt, and the run's own
// bounded retries (evidenceAttempts, with backoff) have already been spent on
// it, so a plane that has not answered by now is not about to.
const logFlushWindow = 3 * time.Second

// flushWindow is how long the kept-back chunk may be offered for. Zero means
// the reviewed logFlushWindow, so every agent New builds flushes at that
// bound; the field exists for the same reason beatInterval's does, that a test
// cannot spend seconds of wall clock watching a window close.
func (agent *Agent) flushWindow() time.Duration {
	if agent.flush > 0 {
		return agent.flush
	}
	return logFlushWindow
}

// windowsLeaveTheReceiptItsOwn states the relation the finishing phase's two
// budgets have to keep: both positive, and the flush strictly shorter than the
// window the receipt travels in, so a flush that spends every millisecond it
// is given still cannot reach into the receipt's.
//
// It is held in a test over this package's own constants rather than checked
// at runtime, because both values are constants here and the regression it
// names is an edit raising logFlushWindow to or past requestLimit. Nothing at
// runtime would notice that: the receipt would simply start failing on planes
// that stall, which is the failure this file exists to end.
func windowsLeaveTheReceiptItsOwn(flush, receipt time.Duration) bool {
	return flush > 0 && receipt > 0 && flush < receipt
}
