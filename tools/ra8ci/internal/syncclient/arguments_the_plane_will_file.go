// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
	"strings"
	"unicode/utf8"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// maxUploadableArguments is local_runs.arguments' count bound, restated
// rather than imported for the same reason every other bound in this
// package is: internal/store does not depend on this package, and a door
// that can only refuse what a live plane would refuse has to know the
// number the plane holds. store.validateLocalRun refuses more than 64.
const maxUploadableArguments = 64

// ErrUnfilableArguments is the refusal of a record whose arguments the
// plane has no column to file.
var ErrUnfilableArguments = errors.New("local record carries arguments the plane will not file")

// entry.Args is the last unexamined field the sweep posts that the far end
// judges without a catalog. It is frozen before the first command runs and
// pinned by spool.checkFinishMatchesStart, and spool.BeginWithMetadata now
// holds it at the freeze (#1950). That is the freeze, not the file:
// spool.Pending reads the record back off disk through json.Unmarshal,
// which fills whatever the file holds, so a record written by an older
// client, or a file edited between the freeze and the sweep, is uploaded
// exactly as written. Same argument as the task-name door beside it.
//
// THE RULE, the store's own: at most 64 elements
// (store.validateLocalRun), each valid UTF-8 with no NUL
// (argumentsAJSONBColumnCanHold). IngestLocalRun json.Marshals the slice
// into local_runs.arguments, jsonb NOT NULL CHECK jsonb_typeof = 'array'
// (0005_offline_sync.sql). The two spellings fail differently and the
// quiet one is worse. A NUL marshals to the escape \u0000, well-formed
// JSON that Postgres jsonb refuses outright, so the record dies inside the
// ingest transaction and is reported as an unavailable store rather than
// the invalid record it is. Invalid UTF-8 json.Marshal does not refuse at
// all: it substitutes U+FFFD per bad byte and the row commits, so the
// arguments on file are not the arguments the task was run with, and this
// is a record read later to say what was executed.
//
// DELIBERATELY NARROWER than the server's rule, the same cut the spool
// makes: server.offlineInput calls definition.ValidateArguments, which
// holds the list to the task's own schema and to catalog.ValidArgumentValue
// (printable ASCII, no shell metacharacters). None of that can be said
// here, because a spooled record carries the catalog DIGEST and not the
// catalog, and a client guessing at it would refuse evidence the plane
// would have taken. Tabs, newlines, empty elements and text outside ASCII
// are ordinary inside a value handed to a child process and are accepted,
// each pinned by test.
//
// *** HONESTY: the store refuses both spellings, so nothing unfilable was
// reaching the column. What the refusal buys is where the sweep stops.
// Refused there, the client reads "upload local <id> returned HTTP 400"
// with no field named, indistinguishable from a server that is merely
// unwell, and that error ends the sweep, so every unsynced record behind it
// in the outbox waits on this pass and on every pass after it. Refused
// here, the operator is told which record and which argument.
func checkUploadedArgumentsAreOnesThePlaneWillFile(entry spool.Entry) error {
	if len(entry.Args) > maxUploadableArguments {
		return fmt.Errorf("%w: %d arguments, the plane files %d",
			ErrUnfilableArguments, len(entry.Args), maxUploadableArguments)
	}
	for i, argument := range entry.Args {
		if !utf8.ValidString(argument) {
			return fmt.Errorf("%w: argument %d is not valid UTF-8", ErrUnfilableArguments, i)
		}
		if strings.ContainsRune(argument, 0) {
			return fmt.Errorf("%w: argument %d carries a NUL", ErrUnfilableArguments, i)
		}
	}
	return nil
}
