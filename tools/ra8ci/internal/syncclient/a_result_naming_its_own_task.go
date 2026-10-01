// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// ErrResultNamesAnotherTask is the refusal of a local record whose execution
// result names a task other than the one the record was frozen for.
var ErrResultNamesAnotherTask = errors.New("local record carries a result naming another task")

// A record states its task twice. Entry.Task is frozen before the first
// command runs and held there by every door this outbox has: Begin writes it,
// checkFinishMatchesStart refuses a terminal record that states a different
// one, and Pending re-applies that rule against the start record on disk.
// Result.TaskName is the executor's own word for what it ran, written once at
// the far end of the attempt (executor.runReviewed fills it from the task it
// was handed), and nothing on this host ever reads it back.
//
// So the two can disagree, and only the server notices. offlineInput refuses
// the pair outright (entry.Result.TaskName != "" && != entry.Task, alongside
// its catalog lookup) because the record is the only thing durable history
// gets: the run row is keyed on Entry.Task, while the result carrying another
// name is the evidence filed under it, and a reader later asking what the
// attempt actually executed has two answers and no way to choose.
//
// The ordinary way it happens is not an attack. Result is a value the caller
// hands to Finish, so a caller looping over reviewed tasks and reusing one
// result variable, or retrying a task and finishing the previous attempt's
// result against the new record, produces exactly this: an honest, terminal,
// schema-v2 record with a complete source identity, stamps in order and steps
// inside the envelope, naming two different tasks.
//
// *** HONESTY: the server refuses it either way, so nothing contradictory was
// reaching the database. What the refusal buys is the same thing the
// unstated-identity, envelope and record-size doors buy around it: where the
// sweep stops and what it says. Refused there, the client reads back "upload
// local <id> returned HTTP 400", which is also what a server that is merely
// unwell says, and that error ends the whole sweep. Pending hands a record
// back until a synced marker sits beside it, so the same record is read,
// posted and refused on every pass after it, and every unsynced record behind
// it in the outbox waits behind it forever. Refused here, the operator is told
// which record and which two names, before the bytes leave the host.
//
// An EMPTY TaskName is deliberately acceptable, matching the server rather
// than being stricter than it. A result carrying no name claims nothing about
// what ran, and the one place that shape is written on purpose is a record
// whose attempt came apart before the executor filled it in; refusing it here
// would strand evidence the server would have taken. This door judges a
// disagreement, not an omission.
func checkUploadedResultNamesItsTask(entry spool.Entry) error {
	if entry.Result == nil || entry.Result.TaskName == "" {
		return nil
	}
	if entry.Result.TaskName != entry.Task {
		return fmt.Errorf("%w: record states task %q, result states %q",
			ErrResultNamesAnotherTask, entry.Task, entry.Result.TaskName)
	}
	return nil
}
