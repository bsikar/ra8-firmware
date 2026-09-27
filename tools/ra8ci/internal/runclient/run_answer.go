// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// checkAnsweredRun holds a run the plane answered with to the run it was
// asked about: the identifier the caller named, and a state the run machine
// knows.
//
// The identifier half was already stated at both doors. The state half was
// not: Get and Cancel asked only that State be non-empty, so any string the
// wire carried was handed back as the state of the run. checkSubmitReceipt, in
// this same package, holds a SUBMISSION receipt to store.KnownRunState and
// says why in writing: the vocabulary of run states lives in the machine and
// nowhere else, and telling a state the plane can be in from one no run ever
// carries needs it. A submitter got that judgement on the first answer about a
// run and lost it on every answer after.
//
// It is the answers after the first that a caller acts on. Submit is one
// request; Get is the poll loop a CI job sits in until the run ends, and that
// loop is written against the machine's vocabulary, because there is no other:
// it waits while the state is queued or running and stops on terminal. A state
// outside the machine matches none of those arms, so the loop neither ends nor
// reports anything wrong, and keeps polling a run it can no longer describe
// until the job's own deadline kills it. Cancel is worse in a quieter way: its
// own rule reads State against the literal "terminal" to decide whether a
// cancellation with no CancelRequestedAt is an honest answer about an
// already-closed run, so it was already spending the machine's vocabulary on a
// string it never held to the machine.
//
// The rule refuses rather than repairs. A state the machine does not know
// means the plane and this client disagree about what a run can be, which is a
// schema change that never reached one of them, and no substitute state this
// client could pick would be true of the run. It names the state, bounded, the
// way checkSubmitReceipt does: the body is capped at a megabyte and the error
// is read by a person.
//
// What it deliberately does not do is judge the state against the run's other
// fields. Whether a terminal run must carry EndedAt, or a running one
// StartedAt, is the plane's invariant over its own rows, and a client that
// refused an answer on those grounds would be refusing runs that exist.
func checkAnsweredRun(run store.Run, id string) error {
	if run.ID != id || !store.ValidID(run.ID) {
		return fmt.Errorf("run API answered about run %q, not the run requested", boundedForError(run.ID))
	}
	if !store.KnownRunState(run.State) {
		return fmt.Errorf("run API answered with run state %q, which is not a run state of the plane",
			boundedForError(run.State))
	}
	return nil
}

// boundedForError cuts a server-supplied string down to something a person can
// read in one line. The response body is already capped, so this is about the
// error, not about memory.
func boundedForError(value string) string {
	const maximum = 64
	if len(value) <= maximum {
		return value
	}
	return value[:maximum] + "..."
}
