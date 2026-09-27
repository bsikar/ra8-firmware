// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
	"unicode/utf8"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// maxFilableExecutorErrorBytes is local_runs.executor_error's bound,
// restated rather than imported: internal/store does not depend on this
// package and this package does not depend on it for the rule, only for the
// receipt type.
const maxFilableExecutorErrorBytes = 1024

// ErrUnfilableExecutorError is the refusal of a record carrying a failure
// message the plane has no column wide enough to file it with.
var ErrUnfilableExecutorError = errors.New("local record carries an executor error the plane will not file")

// entry.Error is the only account durable history keeps of WHY a local
// attempt came apart, and the sweep never looked at it. Finish writes
// runErr.Error() into it and server.offlineInput copies it verbatim into
// store.LocalRunInput.ExecutorError (offline.go:115), which lands in
// local_runs.executor_error, text NOT NULL DEFAULT ” (0005_offline_sync.sql).
// Every other string on a record is chosen from a reviewed definition or
// read off the checkout; this one is whatever a failing executor wrapped
// into an error, and a child's own output is an ordinary thing to wrap.
//
// THE RULE is the store's, restated: at most 1024 bytes, valid UTF-8, no
// NUL (validateLocalRun local_sync.go:208-209, by way of
// executorErrorATextColumnCanHold). A Postgres text column holds no NUL and
// no invalid UTF-8 whatever its length, and this one is bounded on top of
// that.
//
// WHAT THIS DOOR ACTUALLY CATCHES, stated plainly rather than implied. The
// freeze door (spool.checkTheErrorIsOneThePlaneWillFile) already refuses the
// NUL and the invalid byte on this host, and it DELIBERATELY DOES NOT BOUND
// THE LENGTH, on the reasoning that a long but readable message is still
// evidence a person can act on. That leaves the length live here and
// nowhere else on the client: an error of 1025 bytes is frozen willingly,
// passes the record-size door (maxUploadedRecordBytes is 256 KiB, four
// hundred times wider), and is refused by the store, which is the one
// outcome the sweep doors exist to prevent. Note too that json.Unmarshal
// coerces invalid UTF-8 to U+FFFD when Pending reads the record back, so
// the invalid-byte branch below is a cheap restatement for a record this
// process built rather than read; the NUL, which is valid UTF-8 and
// survives \u0000 in JSON, and the length are what genuinely arrive here.
//
// Asked after the record-size door, not beside the record-only doors: a
// message is the one field wide enough to carry a record over the offline
// door by itself, and a record oversized on the whole is reported as that.
//
// The cost of leaving it to the far end is the usual one: the store refuses
// the record inside the ingest transaction, the client reads an opaque
// failure with no field named, and SyncPending returns on it, so every
// unsynced record behind this one in the outbox waits on this pass and on
// every pass after it. Refused here, the operator is told which record and
// which field, and the rest of the outbox goes.
//
// DELIBERATELY NOT a claim about WHEN a message may be present. The store
// refuses an error beside a success (local_sync.go:218), but the server
// never files that pair: offlineInput's switch reads entry.Error != "" first
// (offline.go:127) and classifies the attempt incomplete_evidence, which is
// how a run that came apart before the executor could measure it reaches
// history at all. Refusing it here would throw away that evidence.
func checkUploadedExecutorErrorIsOneThePlaneWillFile(entry spool.Entry) error {
	message := entry.Error
	switch {
	case len(message) > maxFilableExecutorErrorBytes:
		return fmt.Errorf("%w: the message is %d bytes, the plane files %d",
			ErrUnfilableExecutorError, len(message), maxFilableExecutorErrorBytes)
	case !utf8.ValidString(message):
		return fmt.Errorf("%w: the message is not valid UTF-8", ErrUnfilableExecutorError)
	}
	for _, char := range message {
		if char == 0 {
			return fmt.Errorf("%w: the message holds a NUL", ErrUnfilableExecutorError)
		}
	}
	return nil
}
