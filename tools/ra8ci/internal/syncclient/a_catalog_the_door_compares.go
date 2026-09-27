// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// ErrUnstatedCatalogDigest is the refusal of a local record that does not name
// the reviewed catalog it was frozen against.
var ErrUnstatedCatalogDigest = errors.New("local record states no usable catalog digest")

// A record's catalog digest is the only thing on it that says which reviewed
// task list the attempt was begun under. spool.begin freezes it before the
// first command runs and refuses to begin at all without a 64-hex digest
// (`!hexDigest(digest, 64)`), and checkFinishMatchesStart holds the terminal
// record to the start record's copy of it. What neither of those covers is the
// record this sweep actually reads: Pending unmarshals the file on disk, which
// fills whatever the file holds, and every door this package already keeps
// judges some other field.
//
// The far end asks two different questions of it. server.offlineInput compares
// it to the catalog that server is running (`entry.CatalogDigest !=
// cat.Digest()`), which needs that catalog and cannot be asked here. Then
// store.validateLocalRun asks the shape, `hexSHA.MatchString(in.CatalogSHA256)`
// against `^[0-9a-f]{64}$`, before the row is written. Only the second is a
// rule about the record alone, so only the second is restated here.
//
// *** HONESTY: both refusals already happen, so nothing shapeless was reaching
// local_runs.catalog_sha256. What this door buys is where the sweep stops and
// what it says, the same thing every record-only door beside it buys.
// SyncPending turns any non-200 into a returned error that ends the sweep, and
// Pending re-offers a record until a synced marker sits beside it, so one
// record the server will always refuse is read, posted and refused on every
// pass while every record behind it in the outbox waits. The sentence the
// operator reads is "upload local <id> returned HTTP 400", which is also what
// a server that is merely unwell says. Refused here, they are told which
// record and what is wrong with its digest, before the bytes leave the host.
//
// DELIBERATELY NOT a claim about WHICH catalog. A record frozen against a
// catalog this server no longer runs is a real disagreement between two hosts,
// the server is the only party holding both sides of it, and a client that
// guessed at it would refuse evidence the plane would have taken. This door
// asks only whether the record names a digest at all.
func checkUploadedCatalogDigestIsStated(entry spool.Entry) error {
	if !hexOfLength(entry.CatalogDigest, 64) {
		return fmt.Errorf("%w: catalog digest %q is not a 64-hex SHA-256",
			ErrUnstatedCatalogDigest, entry.CatalogDigest)
	}
	return nil
}
