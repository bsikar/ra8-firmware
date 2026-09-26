// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// ErrUnstatedSourceIdentity is the refusal of a record whose schema version
// claims a source identity the record does not carry.
var ErrUnstatedSourceIdentity = errors.New("local record states no usable source identity")

// A record's schema version IS its claim to carry a source identity. The
// spool writes version 2 exactly when a repository was named
// (spool.begin: version = 1 when metadata.Source.Repository == ""), and
// BeginWithMetadata holds the whole identity before the first command runs:
// a repository, a 40-hex commit, verification that is one of two words, and
// a snapshot digest present for "verified" and absent for "unverified".
//
// This sweep re-read that claim as a single integer. Everything else about
// the identity was marshalled and posted unexamined, because Pending's job
// is the record's own consistency (the terminal record against the start
// record) rather than the shape of what it says, and the fields come back
// off disk through json.Unmarshal, which fills whatever the file holds.
//
// What the identity decides is not small. It is the provenance the server
// writes into durable history: repository, branch, commit, and the
// verification word that separates evidence from a working tree nobody
// could prove was clean. The spool's own package comment states the rule
// this door has to keep: unverified "is never upgraded to trusted CI
// evidence during upload". A record carrying "verified" with no snapshot
// digest, or "unverified" with one, is that upgrade mid-sentence, and the
// only reader that noticed was the server.
//
// *** HONESTY: the server refuses every shape below too (server.offlineInput
// re-states the verification pair, and store.validateLocalRun re-states it
// again before the insert), so nothing ill-formed was reaching the database.
// What the refusal buys is where and how the sweep stops. Refused there, the
// client reads back "upload local <id> returned HTTP 400" with no field named
// and no way to tell an ill-formed record from a server that is unwell, and
// that error ends the whole sweep, so every other unsynced record in the
// outbox waits behind it on this pass and on every pass after it. Refused
// here, the operator is told which record and which field, before a
// half-formed provenance claim leaves the host. Same precedent as
// boardclient.WithCorrelationID: something the server would refuse is
// refused HERE, loudly, rather than sent.
func checkUploadedSourceIdentityIsStated(entry spool.Entry) error {
	source := entry.Source
	switch {
	case source.Repository == "":
		return fmt.Errorf("%w: no repository", ErrUnstatedSourceIdentity)
	case !hexOfLength(source.CommitSHA, 40):
		return fmt.Errorf("%w: commit %q is not a 40-hex SHA", ErrUnstatedSourceIdentity, source.CommitSHA)
	case source.Verification != "verified" && source.Verification != "unverified":
		return fmt.Errorf("%w: verification %q is neither verified nor unverified",
			ErrUnstatedSourceIdentity, source.Verification)
	case source.Verification == "verified" && !hexOfLength(source.SnapshotSHA256, 64):
		return fmt.Errorf("%w: verified record carries snapshot %q, not a 64-hex SHA",
			ErrUnstatedSourceIdentity, source.SnapshotSHA256)
	case source.Verification == "unverified" && source.SnapshotSHA256 != "":
		return fmt.Errorf("%w: unverified record carries a snapshot digest",
			ErrUnstatedSourceIdentity)
	}
	return nil
}

func hexOfLength(value string, length int) bool {
	if len(value) != length {
		return false
	}
	for _, char := range value {
		if (char < '0' || char > '9') && (char < 'a' || char > 'f') {
			return false
		}
	}
	return true
}
