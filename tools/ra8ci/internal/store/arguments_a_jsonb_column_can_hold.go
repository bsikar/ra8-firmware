// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import "unicode/utf8"

// argumentsAJSONBColumnCanHold reports whether every argument of a local run
// is text the plane can file unchanged.
//
// validateLocalRun bounded the arguments by COUNT alone: at most 64 of them,
// and nothing about any one. They are not free text on the way out either.
// IngestLocalRun json.Marshals the slice and writes it into
// local_runs.arguments, which is jsonb NOT NULL CHECK jsonb_typeof =
// 'array' (0005_offline_sync.sql), and the two spellings that column cannot
// take were never asked about.
//
// A NUL is the one that fails loudly. json.Marshal encodes it as the escape
// \u0000, which is well-formed JSON and which Postgres jsonb refuses
// outright, so the argument reached the INSERT inside the ingest transaction
// and failed there. That turns a malformed argument into a lost upload of a
// run that otherwise happened, reported as an unavailable store rather than
// as the invalid record it is, which is the same failure the task name and
// the step keys were carrying before namesATextColumnCanHold.
//
// Invalid UTF-8 is the one that fails quietly, and is the worse of the two.
// json.Marshal does not refuse it: it substitutes U+FFFD for each bad byte
// and writes the result. The row commits, the receipt comes back, and the
// arguments on file are not the arguments the task was run with. This is a
// record of a run that already happened, read later to say what was
// executed, so a silently rewritten argument is evidence that has been
// altered by the act of storing it.
//
// Nothing else is refused here. Control characters are left alone
// deliberately, though the task name and step keys refuse them: those are
// identities an operator matches a run by, while an argument is a value that
// was handed to a child process, and a tab or a newline inside one is
// ordinary rather than a sign of a malformed record. Length is left alone
// too, since the count bound and the request's own byte limit already answer
// how much can arrive, and jsonb holds anything that fits those.
func argumentsAJSONBColumnCanHold(arguments []string) bool {
	for _, argument := range arguments {
		if !utf8.ValidString(argument) {
			return false
		}
		for _, char := range argument {
			if char == 0 {
				return false
			}
		}
	}
	return true
}
