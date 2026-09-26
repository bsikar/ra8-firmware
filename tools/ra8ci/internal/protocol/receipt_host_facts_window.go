// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkHostFactsBracketTheAttempt holds a receipt's two host snapshots to the
// attempt they describe. The agent reads the runner once before the task and
// once after it (agent.go, the start and end HostFacts handed to
// terminalReceipt), so the pair brackets the run: the start reading is taken at
// or shortly before the attempt starts and the end reading at or shortly after
// it ends. That ordering is the whole reason there are two of them.
//
// Validate judged each snapshot alone. HostFacts.Validate refuses a zero
// capture stamp and nothing about when it was taken, so the two stamps were
// never compared with each other or with the attempt's own window, and a
// receipt could carry an end snapshot taken before the attempt began, or a
// start snapshot taken after it finished, or the pair in the wrong order.
//
// The snapshots are read, not decorated. The store files the end snapshot as
// the attempt's resource sample (store/dispatch.go, recordResourceSample) and
// an operator reads the pair to say what the runner looked like while the work
// ran: free memory before and after, load before and after. A snapshot from the
// wrong side of the attempt answers those questions with a machine state the
// work never saw, and answering them wrongly is worse than refusing.
//
// The rule states only the ordering, not containment. The readings deliberately
// sit slightly outside the window on both sides, since each is taken around the
// run rather than during it, so requiring them inside would refuse every honest
// receipt. What it refuses is a snapshot on the wrong side of the attempt
// entirely, with the clockDisagreement allowance receipt_duration.go already
// fixes for the same reason: the stamps come from separate readings of one wall
// clock.
func checkHostFactsBracketTheAttempt(receipt TerminalReceipt) error {
	start := receipt.HostFactsAtStart.CapturedAt
	end := receipt.HostFactsAtEnd.CapturedAt
	if end.Before(start) {
		return fmt.Errorf("%w: the host snapshot at the end was taken before the one at the start", ErrInvalid)
	}
	if start.After(receipt.EndedAt.Add(clockDisagreement)) {
		return fmt.Errorf("%w: the host snapshot at the start was taken after the attempt ended", ErrInvalid)
	}
	if end.Before(receipt.StartedAt.Add(-clockDisagreement)) {
		return fmt.Errorf("%w: the host snapshot at the end was taken before the attempt started", ErrInvalid)
	}
	return nil
}
