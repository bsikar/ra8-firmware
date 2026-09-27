// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"strings"
	"unicode/utf8"
)

const (
	// maxFilableSourceBytes and maxFilableTaskBytes are the lengths the plane
	// files a local run's identities at: local_runs.repository and .branch are
	// text CHECK (length <= 512) and .task_name is text CHECK (length BETWEEN
	// 1 AND 128) (0005_offline_sync.sql), and store.validateLocalRun refuses
	// anything longer before the insert. Restated rather than imported:
	// neither package depends on the other, and a door that can only refuse
	// what a live plane would refuse has to know the numbers the plane holds.
	maxFilableSourceBytes = 512
	maxFilableTaskBytes   = 128
)

// errUnfilableIdentity names the one thing this rule refuses: an identity
// frozen into a start record that the plane will not file the run under.
var errUnfilableIdentity = errors.New("local run identity the plane will not file")

// checkIdentitiesAreOnesThePlaneWillFile holds the three strings a local run
// is filed under to what the far end will accept from the record afterwards.
//
// This is the argument that landed checkArgumentsAreOnesThePlaneWillFile, one
// field over. BeginWithMetadata reads the repository only for being non-empty
// and the branch and the task name not at all, though all three are frozen
// into the start record, pinned there by checkFinishMatchesStart, and carried
// into store.LocalRunInput by offlineInput exactly as written. The plane
// refuses four spellings of them that this door was not asking about: a
// repository or branch over 512 bytes, a task name over 128 or with
// surrounding whitespace, and in any of the three a NUL, a control character
// or bytes that are not valid UTF-8 (namesATextColumnCanHold).
//
// A Postgres text column holds no NUL and no invalid UTF-8 whatever its
// length, so those two reached the INSERT inside the ingest transaction and
// failed there, and the operator read an unavailable store rather than the
// invalid record it was. The rest of the control range is the reading half,
// the same one the store's own rule states: these are the strings an operator
// matches a run by in history output, and a branch carrying a newline reads as
// two rows in anything line-oriented.
//
// Refusing at Begin is what the timing is worth. The spool freezes this
// metadata before the first command and never revises it, so an identity the
// plane will not file makes the run unuploadable from the moment it starts:
// the task runs to completion, the terminal record is written, and the upload
// comes back 400 when a server is finally reachable. syncclient.SyncPending
// returns on the first upload that is not 200, so that record holds every
// record behind it in the outbox on that pass and every pass after. The branch
// is the likeliest of the three to carry something unexpected, since it is
// read from whatever the local checkout says rather than chosen from a
// reviewed definition.
//
// Stated at BeginWithMetadata only, not in begin. A record written by Begin
// carries schema version 1 and no source at all, and offlineInput refuses
// entry.SchemaVersion != 2 outright, so its identities are never filed by
// anything and there is nothing for this rule to protect there.
func checkIdentitiesAreOnesThePlaneWillFile(task string, source SourceIdentity) error {
	if len(task) < 1 || len(task) > maxFilableTaskBytes {
		return fmt.Errorf("%w: task name is %d bytes, and the plane files 1 to %d",
			errUnfilableIdentity, len(task), maxFilableTaskBytes)
	}
	if strings.TrimSpace(task) != task {
		return fmt.Errorf("%w: task name %q is not the name the plane files it under",
			errUnfilableIdentity, task)
	}
	if len(source.Repository) > maxFilableSourceBytes {
		return fmt.Errorf("%w: repository is %d bytes, and the plane files at most %d",
			errUnfilableIdentity, len(source.Repository), maxFilableSourceBytes)
	}
	if len(source.Branch) > maxFilableSourceBytes {
		return fmt.Errorf("%w: branch is %d bytes, and the plane files at most %d",
			errUnfilableIdentity, len(source.Branch), maxFilableSourceBytes)
	}
	for _, named := range []struct {
		field string
		value string
	}{{"task name", task}, {"repository", source.Repository}, {"branch", source.Branch}} {
		if !textThePlaneCanFile(named.value) {
			return fmt.Errorf("%w: %s holds text a text column cannot carry unchanged",
				errUnfilableIdentity, named.field)
		}
	}
	return nil
}

// textThePlaneCanFile mirrors store.namesATextColumnCanHold: valid UTF-8, and
// no C0, DEL or C1 control character. The store states this about a name it is
// about to write; this package states it about the same name before the run
// that produces it begins.
func textThePlaneCanFile(value string) bool {
	if !utf8.ValidString(value) {
		return false
	}
	for _, char := range value {
		if char < 0x20 || char == 0x7f || (char >= 0x80 && char <= 0x9f) {
			return false
		}
	}
	return true
}
