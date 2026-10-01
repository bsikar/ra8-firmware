// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// errResultNamesAnotherTask names the one thing this rule refuses: a result
// handed to Finish that says it ran a task other than the one the record was
// frozen for.
var errResultNamesAnotherTask = errors.New("execution result names another task")

// checkTheResultNamesTheFrozenTask holds the executor's own word against the
// record's, at the freeze.
//
// A terminal record states its task twice. Entry.Task is frozen before the
// first command runs and held there by every door this package has: Begin
// writes it, checkFinishMatchesStart refuses a terminal record stating a
// different one, and Pending re-applies that rule against the start record on
// disk. Result.TaskName is the executor's own account of what it ran, filled at
// the far end of the attempt by runReviewed from the task it was handed, and
// until now nothing on this host read it back. checkFinishMatchesStart cannot
// catch this: it compares the finishing ENTRY to the started one, and the
// result is a separate argument that never takes part in that comparison.
//
// So the two can disagree and the record is written anyway. The ordinary way it
// happens is not an attack: Result is a value the caller hands to Finish, so a
// caller looping over reviewed tasks and reusing one result variable, or
// finishing the previous attempt's result against a retried record, produces an
// honest-looking terminal record naming two different tasks. The run row is
// keyed on Entry.Task while the result carrying the other name is the evidence
// filed under it, and a reader later asking what the attempt actually executed
// has two answers and no way to choose.
//
// This is the FREEZE, not the sweep, and the two doors are not the same door.
// checkUploadedResultNamesItsTask judges a record read back OFF DISK, where an
// older build's file or an edited one can say anything at all; this one judges
// what the executor in this process just handed over, before the bytes are
// written, so the outbox never holds the pair in the first place. Refused at
// the sweep instead, the record is read, posted, refused with an opaque 400 and
// left in the outbox, and every unsynced record behind it waits on every pass.
//
// An EMPTY TaskName is deliberately acceptable, matching the server
// (offlineInput refuses the pair only when the result names something) rather
// than being stricter than it. A result carrying no name claims nothing about
// what ran, and the one place that shape is written on purpose is an attempt
// that came apart before the executor filled it in; refusing it here would
// throw away the only account history gets of that failure. This door judges a
// disagreement, not an omission.
func checkTheResultNamesTheFrozenTask(entry Entry, result executor.Result) error {
	if result.TaskName == "" || result.TaskName == entry.Task {
		return nil
	}
	return fmt.Errorf("%w: the record was frozen for %q, the result states %q",
		errResultNamesAnotherTask, entry.Task, result.TaskName)
}
