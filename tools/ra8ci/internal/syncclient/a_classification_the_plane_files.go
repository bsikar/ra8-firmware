// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// The three classifications durable history files a local run under. They are
// column constraints, not this build's opinion: migrations/0005_offline_sync.sql
// declares local_runs.tier CHECK (tier IN ('required', 'optional', 'nightly')),
// local_runs.scope CHECK (scope IN ('safe-local-read-only',
// 'safe-local-write-working-tree')) and local_runs.deadline_seconds CHECK
// (deadline_seconds BETWEEN 1 AND 86400), and store.validateLocalRun restates
// all three before the insert.
const (
	minFilableDeadlineSeconds = 1
	maxFilableDeadlineSeconds = 86400
)

var (
	filableTiers  = []string{"required", "optional", "nightly"}
	filableScopes = []string{"safe-local-read-only", "safe-local-write-working-tree"}
)

// ErrUnfilableClassification is the refusal of a local record classified in a
// way durable history has no column value for.
var ErrUnfilableClassification = errors.New("local record states a classification the plane cannot file")

// A record states its tier, its scope and its deadline, and this sweep posted
// all three unexamined. spool.BeginWithMetadata freezes them before the first
// command runs, but it asks only that the tier and the scope be non-empty
// (metadata.Tier == "" || metadata.Scope == ""), and the record this sweep
// reads comes back off disk through json.Unmarshal, which fills whatever the
// file holds.
//
// The far end asks twice. server.offlineInput compares all three to the
// reviewed definition (definition.Tier != entry.Tier, definition.Scope !=
// entry.Scope, definition.DeadlineSeconds != entry.DeadlineSeconds), which
// needs that catalog and cannot be asked here. store.validateLocalRun then
// asks the enumerations and the range, which need nothing but the record, and
// those are the two CHECK constraints the columns themselves carry. Only the
// second is restated here, the same cut this package already made for the
// catalog digest: the shape a column can hold is the client's to state, while
// which reviewed task the record matches is the server's.
//
// *** HONESTY: the server and the store both refuse these shapes already, so
// nothing unfilable was reaching local_runs. What the refusal buys is where
// the sweep stops and what it says. SyncPending returns on the first non-200,
// which ends the sweep, and Pending hands a record back until a synced marker
// sits beside it, so one record the plane will always refuse is read, posted
// and refused on every pass while every unsynced record behind it waits. The
// operator reads "upload local <id> returned HTTP 400", which is also what a
// server that is merely unwell says. Refused here they are told which record
// and which classification, before the bytes leave the host that wrote them.
//
// The deadline is judged as a RANGE and never against the record's own
// envelope. A run that overran its deadline is ordinary evidence, and the
// column holds the deadline the task was begun under, not a measurement of
// what happened.
func checkUploadedClassificationIsOneThePlaneFiles(entry spool.Entry) error {
	if !oneOf(entry.Tier, filableTiers) {
		return fmt.Errorf("%w: tier %q is none of %v", ErrUnfilableClassification, entry.Tier, filableTiers)
	}
	if !oneOf(entry.Scope, filableScopes) {
		return fmt.Errorf("%w: scope %q is none of %v", ErrUnfilableClassification, entry.Scope, filableScopes)
	}
	if entry.DeadlineSeconds < minFilableDeadlineSeconds || entry.DeadlineSeconds > maxFilableDeadlineSeconds {
		return fmt.Errorf("%w: deadline of %ds is outside the %d to %d the column holds",
			ErrUnfilableClassification, entry.DeadlineSeconds,
			minFilableDeadlineSeconds, maxFilableDeadlineSeconds)
	}
	return nil
}

func oneOf(value string, allowed []string) bool {
	for _, candidate := range allowed {
		if value == candidate {
			return true
		}
	}
	return false
}
