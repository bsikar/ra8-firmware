// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// checkSubmitReceipt holds a submission receipt to a run the plane could
// answer with: a well-formed run ID, and a state the run machine knows.
//
// It used to require the state to be "queued", which is only ever true of a
// FRESH admission. A submission carries a required idempotency key, and the
// whole point of that key is the retry after a lost response: the server
// answers such a retry from the key it recorded and returns the run as it
// stands NOW (store.CreateRun replays through GetRun), so the moment the first
// attempt has been claimed the replay says "running", and a finished run says
// "terminal". Both are honest answers to "the run you asked for exists, here
// it is", and both were refused.
//
// The refusal was the expensive part: Submit returns a zero Receipt with the
// error, so the caller lost the run ID of a run that IS admitted and running.
// It cannot then poll it, cancel it or read its logs, and its own retry is
// what created it. Whether that happened turned on timing alone, which is the
// worst version of the bug: the retry works while the queue is slow and fails
// once a runner picks the work up.
//
// The client cannot tell a fresh admission from a replay (both answer 201 with
// the same body shape), so it does not try to: it holds the receipt to what
// every answer must have, and leaves the state to the machine.
func checkSubmitReceipt(receipt Receipt) error {
	if !store.ValidID(receipt.ID) {
		return errors.New("run submission returned a receipt without a valid run ID")
	}
	if !store.KnownRunState(receipt.State) {
		// Name the state, bounded: the body is capped at a megabyte, and an
		// error is read by a person.
		state := receipt.State
		if len(state) > 64 {
			state = state[:64] + "..."
		}
		return fmt.Errorf("run submission returned a receipt in state %q, which is not a run state of the plane", state)
	}
	return nil
}
