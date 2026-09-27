// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// checkOffsetsTheAttemptCouldStamp holds a served log page's monotonic offsets
// to what the attempt that produced them could have stamped: no offset before
// the attempt's own start, and no offset going backwards down a page the
// server returned in sequence order.
//
// Every other field of a served chunk is already judged here. Logs decodes
// DataBase64 and refuses it empty or over 64KiB, recomputes the SHA-256 and
// refuses a mismatch, holds Sequence to a contiguous run from the requested
// cursor, refuses a Stream that is neither stdout nor stderr, and checks the
// page's own cursor arithmetic. MonotonicOffsetNS was the one field of
// store.LogRecord handed to the caller unjudged.
//
// It is not a field the client can afford to take on faith, because the store
// does not: AttemptLogs refuses a NEGATIVE offset while reading its own rows,
// alongside the same stream, size and digest rules this client repeats on the
// wire. Those rules are repeated here deliberately, and the reason is stated
// in Logs itself by the digest check: the client verifies the bytes rather
// than trusting the answer, so a field the far end guards on its own read path
// and this end does not is the one place a page can carry something the store
// would have refused.
//
// What a bad offset costs is quieter than a bad digest, which is why it is
// worth naming. Sequence is the order of the page; the offset is the only
// thing that places a chunk in TIME relative to the attempt, and it is the
// only key that can interleave two streams, since stdout and stderr carry
// separate byte streams that a reader has to put back into one transcript.
// A negative offset claims output from before the attempt began, and a
// backwards step claims output that arrived earlier than output already read.
// Either one silently reorders a transcript an operator is reading to work out
// what failed, and nothing about the page looks wrong while it happens.
//
// NON-DECREASING, not strictly increasing, and this matters: the agent log
// path in store.Dispatch inserts every chunk with monotonic_offset_ns = 0, so
// a whole page of zeros is the ordinary shape for agent-submitted output and
// must pass. The rule refuses only movement that no clock can make.
//
// WITHIN THE PAGE ONLY. The client sees one page at a time and holds no memory
// of the last chunk of the previous page, so it cannot judge the seam between
// pages, and it does not pretend to: a rule that compared against a remembered
// offset would refuse a legitimate resume from a cursor the caller chose. The
// seam is the store's to hold, over rows it owns.
//
// It refuses rather than repairs, like every other door in this package. An
// offset the attempt could not have stamped means the page says something
// about the attempt's own clock that is not true, and no substitute offset the
// client could invent would be true either.
func checkOffsetsTheAttemptCouldStamp(page store.LogPage) error {
	previous := int64(0)
	for index, chunk := range page.Chunks {
		if chunk.MonotonicOffsetNS < 0 {
			return fmt.Errorf("run log chunk %d is stamped %dns before its attempt began",
				chunk.Sequence, chunk.MonotonicOffsetNS)
		}
		if index > 0 && chunk.MonotonicOffsetNS < previous {
			return fmt.Errorf("run log chunk %d is stamped at %dns, before chunk %d at %dns",
				chunk.Sequence, chunk.MonotonicOffsetNS, page.Chunks[index-1].Sequence, previous)
		}
		previous = chunk.MonotonicOffsetNS
	}
	return nil
}
