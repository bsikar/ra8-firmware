// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

const (
	// legacySchemaVersion is the one shape this sweep deliberately leaves on
	// disk: a record begun before source identity was frozen.
	legacySchemaVersion = 1
	// uploadableSchemaVersion is the shape the spool writes today and the only
	// one the server's offline door accepts.
	uploadableSchemaVersion = 2
)

// ErrUnknownSchemaVersion is the refusal of a local record written in a shape
// this build cannot read.
var ErrUnknownSchemaVersion = errors.New("local record states a schema version this build does not know")

// The sweep sorted every pending record into two piles by asking one question:
// is the schema version 2? Everything else was counted as Quarantined, a field
// whose own comment says what the pile is ("schema-v1 records remain unsynced,
// never reclassified"), and then skipped.
//
// Only version 1 is that pile. The spool writes exactly two versions, 1 when
// no repository was named and 2 otherwise (spool.begin), so a record carrying
// anything else was not written by this build. The two ways that happens point
// in opposite directions and neither is legacy. A record written by a NEWER
// ra8ci is a shape whose fields this build cannot be sure it reads correctly;
// the whole point of the version is to say so. A record carrying 0 is a file
// whose schema_version did not survive whatever produced it, so what the rest
// of its fields mean is equally unknown. Both are reported to the operator as
// old records that will never sync, which is a sentence about a decision
// somebody already made, and the outbox is the only place either one exists.
//
// A silent skip is also permanent. Pending() returns a record until a synced
// marker sits beside it, so a version this sweep does not recognise is read,
// counted under the wrong name and left for the next pass to read, count and
// leave again, forever, at a number the operator has no reason to look at.
//
// So the sweep refuses it, names the record and the version, and stops. That
// is the same answer this package already gives a half-formed source identity:
// something it cannot honestly upload is refused HERE, loudly, rather than
// sent or quietly dropped. Stopping the sweep is the fail-closed direction.
// The unknown record is the one thing on this host that nothing else can
// judge, and the records behind it are unharmed by waiting, while every pass
// that continues past it buries it one line deeper in a count that says
// "legacy".
func checkSchemaVersionIsKnown(entry spool.Entry) error {
	switch entry.SchemaVersion {
	case legacySchemaVersion, uploadableSchemaVersion:
		return nil
	default:
		return fmt.Errorf("%w: schema version %d is neither %d nor %d",
			ErrUnknownSchemaVersion, entry.SchemaVersion, legacySchemaVersion, uploadableSchemaVersion)
	}
}
