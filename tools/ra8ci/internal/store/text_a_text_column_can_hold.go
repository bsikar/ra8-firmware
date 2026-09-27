// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import "unicode/utf8"

// namesATextColumnCanHold reports whether an identity string a local run is
// filed under is text this store can actually write and a reader can read
// back. validateLocalRun bounded both of them by LENGTH alone: a task name is
// asked to be 1..128 bytes with no surrounding whitespace, a step key to be
// 1..128 bytes and unique within the run, and every byte in between was
// accepted. A name carrying a NUL, a newline, an escape sequence, or bytes
// that are not valid UTF-8 at all passed the gate.
//
// The migrations bound the same two columns the same way and no further:
// local_runs.task_name and local_run_steps.step_key are text CHECK (length
// BETWEEN 1 AND 128) (0005_offline_sync.sql), and a Postgres text column holds
// no NUL byte and no invalid UTF-8 whatever its length. So those two spellings
// were not refused at the gate that exists to refuse them; they reached the
// INSERT inside the ingest transaction and failed there, which turns a
// malformed name into a lost upload of a run that otherwise happened, reported
// as an unavailable store rather than as the invalid record it is.
//
// The rest of the control range is the reading half. These are the strings an
// operator matches a local run by, in history output and on the command line,
// and a key carrying a newline reads as two steps in anything line-oriented
// while one carrying an escape sequence rewrites the terminal that prints it.
// C1 is refused with C0 for the same reason the agent boundary refuses it
// (protocol, stepNameCanBeFiled): a name that renders as nothing is a name no
// one can match by reading it, and these keys are compared byte for byte
// against the rows already stored.
//
// The rule states what the store requires rather than borrowing the catalog's
// far narrower alphabet. Every honest record's task name and step keys come
// from a reviewed definition (server/offlineInput matches both against the
// catalog before building the input), but LocalRunInput is an exported struct
// and validateLocalRun is the store's own door: it should enforce its own
// contract rather than inherit it from whoever built the input, which is the
// same reason ValidateArtifactSet stopped trusting its caller to hand it one
// attempt.
func namesATextColumnCanHold(value string) bool {
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
