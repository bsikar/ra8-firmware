// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"strings"
	"unicode/utf8"
)

// The terminal reason is the only free text a HIL attempt sends back, and it
// is written with the completion, in the same transaction that takes the
// attempt out of "running". The store bounds it at 1024 (validAttemptResult
// in internal/store/attempts.go reads len(in.Reason), which is bytes), and
// the column it lands in is ordinary text in a UTF-8 database, so the bytes
// have to decode.
//
// RunHILAttempt cut an over-long reason with completion.Reason[:1024], which
// is a byte slice. A reason is an error string, and error strings carry
// whatever the failure carried: a manifest path, a board model, a message
// from a step's own output. The moment one of those holds a multi-byte rune
// straddling byte 1024, the cut lands inside the rune and the reason stops
// being text: the write is refused for the encoding, CompleteHILAttempt
// returns that error, and RunHILAttempt hands its caller a completion that
// was never persisted. The attempt stays "running" with the board still
// recorded as held, and nothing frees it before the lease expires and the
// reviewed recovery sequence runs.
//
// So the failure mode selects for exactly the wrong case. A short, ordinary
// reason is written. A long one, which is what a deeply nested or badly
// behaved failure produces, is the one at risk of being dropped, and it is
// dropped in the way that costs a board rather than a line of text.
//
// Two conditions, because neither implies the other: the result is at most
// maxCompletionReason bytes, and the result decodes. A reason that was
// already not text before the cut is held to the same condition here rather
// than left for the database to find, because there is no honest reading
// under which invalid bytes in an error string are worth a lost attempt.
// That replacement is lossy and says so: the bytes it drops were never
// readable.

// maxCompletionReason is the longest terminal reason a completion can carry,
// in bytes, matching the bound the store applies.
const maxCompletionReason = 1024

// boundedReason returns reason as text a completion record can hold: valid
// UTF-8, at most maxCompletionReason bytes, and never cut inside a rune. It
// keeps as much of the reason as fits, so the cut costs at most the three
// bytes of a straddling rune.
func boundedReason(reason string) string {
	if utf8.ValidString(reason) && len(reason) <= maxCompletionReason {
		return reason
	}
	if !utf8.ValidString(reason) {
		reason = strings.ToValidUTF8(reason, "")
	}
	if len(reason) <= maxCompletionReason {
		return reason
	}
	cut := maxCompletionReason
	for cut > 0 && !utf8.RuneStart(reason[cut]) {
		cut--
	}
	return reason[:cut]
}
