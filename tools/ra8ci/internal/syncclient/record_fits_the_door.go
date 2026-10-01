// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
)

// maxUploadedRecordBytes is the largest request body the control plane's
// offline door will read. It is the server's own bound, not a second opinion:
// server.ingestOffline wraps the request body in an http.MaxBytesReader of
// 256<<10 before it reads a byte, and this client posts exactly the marshalled
// record the server counts, so the two numbers are counts of the same bytes.
const maxUploadedRecordBytes = 256 << 10

// ErrRecordExceedsOfflineDoor is the refusal of a local record larger than the
// server's offline door can read.
var ErrRecordExceedsOfflineDoor = errors.New("local record is larger than the offline door reads")

// Nothing bounds how big a spooled record gets. Its size is decided by fields
// the local host writes without a ceiling: the reviewed arguments, one step row
// per step carrying two digests and two byte counts, and Error, which is
// whatever text the executor handed back when the attempt came apart. A record
// can therefore be honest, terminal, schema-v2 and carry a complete source
// identity, pass every door this package already holds it to, and still be
// larger than the server will read.
//
// What that costs is not one record. MaxBytesReader makes the server's ReadAll
// fail, so ingestOffline answers HTTP 400 "offline record exceeds limit or is
// unreadable", and SyncPending turns any non-200 into a returned error, which
// ends the sweep where it stands. Pending() hands a record back until a synced
// marker sits beside it, so the oversized record is read again on the next
// pass, marshalled again, posted again, refused again, and every unsynced
// record behind it in the outbox waits behind it again. The outbox stops
// draining at the first record too big to send, permanently, and the sentence
// the operator reads is "upload local <id> returned HTTP 400", which is also
// what a server that is merely unwell says.
//
// *** HONESTY: the record does not become syncable by being refused here. It
// is unsendable either way, and this rule does not shrink, split or drop it.
// What the refusal buys is the same thing the unstated-identity door buys one
// check above: the sweep stops BEFORE the bytes leave the host, and it stops
// naming the record, its size and the bound it missed, so the operator is
// reading a fact about their own outbox rather than a status code from a door
// they cannot see. Stopping rather than skipping is deliberate and matches the
// rest of this package: a record this client cannot honestly upload is refused
// loudly, never sent and never counted quietly into a pile nobody reads.
func checkUploadedRecordFitsTheOfflineDoor(body []byte) error {
	if len(body) > maxUploadedRecordBytes {
		return fmt.Errorf("%w: %d bytes, over the %d the server reads",
			ErrRecordExceedsOfflineDoor, len(body), maxUploadedRecordBytes)
	}
	return nil
}
