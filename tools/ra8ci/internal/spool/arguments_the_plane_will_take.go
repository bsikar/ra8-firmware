// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"unicode/utf8"
)

// maxLocalRunArguments is the number of arguments the plane files for a local
// run. It is restated here rather than imported: internal/store does not
// depend on this package and this package does not depend on it, and a door
// that can only refuse what a live plane would refuse has to know the number
// the plane holds. store.validateLocalRun refuses len(in.Arguments) > 64
// (local_sync.go), and the catalog cannot produce more in any case: a schema
// binds at most MaxPositionalArguments + MaxFlagArguments = 24 elements.
const maxLocalRunArguments = 64

// errUnfilableArguments names the one thing this rule refuses: an argument
// list frozen into a record that the plane will not file.
var errUnfilableArguments = errors.New("local run arguments the plane will not file")

// checkArgumentsAreOnesThePlaneWillFile holds the arguments frozen before a
// local run to what the far end will accept from the record afterwards.
//
// BeginWithMetadata is the door that freezes reviewed metadata before the
// first command runs, and it reads every other field it is handed: the
// repository, the commit and snapshot digests, the verification word, the
// tier, the scope and the deadline. It said nothing at all about Args, which
// it copies straight into the start record, where checkFinishMatchesStart
// then pins them: the terminal record must carry the same list the start
// record froze, so whatever arrives here is what is eventually uploaded.
//
// Two spellings cannot be filed. store.validateLocalRun bounds the list at 64
// elements, and argumentsAJSONBColumnCanHold refuses a NUL or invalid UTF-8
// in any one of them, because IngestLocalRun json.Marshals the slice into
// local_runs.arguments, jsonb NOT NULL CHECK jsonb_typeof = 'array'
// (0005_offline_sync.sql). A NUL marshals to \u0000, which Postgres jsonb
// refuses outright; invalid UTF-8 is not refused by json.Marshal at all, it
// is silently rewritten to U+FFFD per bad byte, so the arguments on file stop
// being the arguments the task ran with.
//
// The cost of learning that late is the whole point of refusing here. The
// spool exists so a disconnected host keeps its evidence: the attempt is
// frozen, the task runs to completion, the terminal record is written, and
// only when a server is finally reachable does the upload come back 400. At
// that point the work is already spent and unrecoverable, and it is worse
// than one lost record: syncclient.SyncPending returns on the first upload
// that is not 200, so an unfilable record at the front of the outbox ends the
// sweep and holds every record behind it on that pass and on every pass
// after. Refusing at Begin costs a run that could never have been filed and
// reports it on the host, in the moment, naming the argument.
//
// DELIBERATELY NARROWER than the server's rule. offlineInput calls
// definition.ValidateArguments, which holds the list to the task's own schema
// and to catalog.ValidArgumentValue (printable ASCII, no shell
// metacharacters, nothing that looks like a flag). None of that can be said
// here: a spool record carries the catalog DIGEST, not the catalog, so this
// package cannot know which task binds which arguments. What it states is
// only what is true of every task's arguments at every far end, which is the
// two rules the columns themselves impose. A record this door accepts can
// still be refused by the catalog; a record it refuses could never have been
// filed by anything.
func checkArgumentsAreOnesThePlaneWillFile(arguments []string) error {
	if len(arguments) > maxLocalRunArguments {
		return fmt.Errorf("%w: %d arguments, and the plane files at most %d",
			errUnfilableArguments, len(arguments), maxLocalRunArguments)
	}
	for index, argument := range arguments {
		if !utf8.ValidString(argument) {
			return fmt.Errorf("%w: argument %d is not valid UTF-8", errUnfilableArguments, index)
		}
		for _, char := range argument {
			if char == 0 {
				return fmt.Errorf("%w: argument %d holds a NUL", errUnfilableArguments, index)
			}
		}
	}
	return nil
}
