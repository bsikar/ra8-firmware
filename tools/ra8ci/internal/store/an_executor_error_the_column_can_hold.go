// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import "unicode/utf8"

// executorErrorATextColumnCanHold reports whether the executor error of a
// local run is text this store can write unchanged.
//
// validateLocalRun bounded it by LENGTH alone: at most 1024 bytes, and every
// byte in between accepted. It is not free text on the way out.
// IngestLocalRun writes the string straight into local_runs.executor_error,
// text NOT NULL DEFAULT ” (0005_offline_sync.sql), and a Postgres text
// column holds no NUL byte and no invalid UTF-8 whatever its length. So the
// two spellings that column cannot take reached the INSERT inside the ingest
// transaction and failed there, which turns a malformed message into a lost
// upload of a run that otherwise happened, reported as an unavailable store
// rather than as the invalid record it is. That is the failure the task name
// and the step keys were carrying before namesATextColumnCanHold, and the
// arguments before argumentsAJSONBColumnCanHold, on the last field of this
// input that is neither an identity nor a number.
//
// This field is the likeliest of the three to carry either spelling, which
// is why it is worth stating rather than leaving to the column. It is the
// only string in a local run that is not chosen from a reviewed definition:
// spool.Finish writes runErr.Error() into it (spool.go:178), so whatever a
// failing executor wrapped into an error travels here, and a child's own
// output is a common thing to wrap. Bytes read off a pipe are not UTF-8
// because a Go string holds them.
//
// The rule is deliberately narrower than the one the task name and step keys
// answer to, and matches the arguments instead. Control characters are left
// alone: this is a message a person reads, a wrapped executor failure runs to
// several lines, and a newline inside one is ordinary rather than a sign of a
// malformed record. The 1024-byte bound already answers how much can arrive,
// so no length rule is restated here.
func executorErrorATextColumnCanHold(message string) bool {
	if !utf8.ValidString(message) {
		return false
	}
	for _, char := range message {
		if char == 0 {
			return false
		}
	}
	return true
}
