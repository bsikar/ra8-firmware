// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"

// endHostFacts answers with the host snapshot the terminal receipt carries for
// the end of the attempt, and whether that snapshot is a reading genuinely
// taken after the work finished.
//
// The receipt is the only word the plane ever gets about how an attempt ended,
// and nothing reconstructs it: without it the attempt sits under its fence
// until the lease expires and an operator reads a runner that went silent. The
// artifact window and the log flush window each exist to stop an optional half
// of the finishing phase spending the receipt's budget, and both say so.
//
// The end-of-attempt host reading was left able to destroy the receipt
// outright. execute read the runner once more and returned that error, so a
// /proc/meminfo or /proc/loadavg read that failed AFTER the task ran, after the
// logs were delivered and the artifacts uploaded, threw away the whole report:
// the outcome, the child exit, every step summary, the final log sequence. The
// reading fails exactly where it costs the most, on a host short of memory or
// under the load the attempt itself just created, which is the attempt whose
// outcome an operator most wants to read. A clock stepped backwards between the
// two readings is the same loss by another route: Validate refuses a receipt
// whose end snapshot predates its start snapshot, so the receipt is built and
// then discarded at the door.
//
// The start reading stands in when there is no honest end reading, because it
// is the one snapshot of this runner the attempt is certain to hold and it
// brackets the attempt by construction. It is NOT presented as a measurement of
// the end: withoutEndReading clears the evidence flag on the receipt that
// carries it, so the plane, which believes that flag over everything else,
// treats the attempt as unverified rather than green.
//
// A measurement is not repaired here. A reading that failed is not re-read and
// a reading that arrived invalid is not patched: this rule only decides which
// snapshot a receipt can honestly carry, so the report survives the reading.
func endHostFacts(start, measured protocol.HostFacts, err error) (protocol.HostFacts, bool) {
	if err != nil || measured.Validate() != nil {
		return start, false
	}
	// The receipt refuses an end snapshot taken before the start one, so a
	// reading this receipt cannot carry is no better than one never taken.
	if measured.CapturedAt.Before(start.CapturedAt) {
		return start, false
	}
	return measured, true
}

// withoutEndReading states, on the receipt itself, that its end snapshot is the
// start reading repeated rather than a measurement of the finished attempt.
//
// Clearing the evidence flag is the whole of the claim: it is the field the
// plane reads to decide whether a green attempt can be believed, and the field
// the error code is held against. The outcome follows it because a receipt
// cannot say both, succeeded is defined as complete evidence and Validate
// refuses the pair. A timed-out or cancelled attempt keeps its outcome, which
// is a verdict about the child rather than about the evidence.
//
// No error code is invented for it. The vocabulary an agent may state is closed
// (executor_error, log_upload_error, artifact_upload_error, no_step_executed)
// and lands verbatim in the attempt's durable result_reason, so a fifth string
// would be unreadable beside the reasons the plane writes itself. An incomplete
// receipt naming no code is a shape the protocol already allows.
func withoutEndReading(receipt protocol.TerminalReceipt) protocol.TerminalReceipt {
	receipt.EvidenceComplete = false
	if receipt.Outcome == "succeeded" {
		receipt.Outcome = "failed"
	}
	return receipt
}
