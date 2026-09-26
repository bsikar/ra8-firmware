// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"slices"
)

// errFinishDisagrees names the one thing this rule refuses: a terminal record
// that states something other than what was frozen before the task ran.
var errFinishDisagrees = errors.New("terminal record disagrees with the record frozen before execution")

// checkFinishMatchesStart holds a terminal record to the start record on disk.
//
// Begin and BeginWithMetadata exist to freeze, before the first command runs,
// what the local checkout could actually prove: the reviewed task, the catalog
// digest it was read from, the tier, scope, deadline and arguments that were
// declared for it, and the source identity, whose verification is documented
// as never being upgraded to trusted CI evidence during upload. Finish then
// wrote whatever the caller handed back, and the only thing it asked of the
// frozen record was that its file still existed (an Lstat whose result was
// discarded). Every frozen field therefore travelled through the caller across
// the whole execution, and the terminal record is the one that is uploaded:
// syncclient.SyncPending marshals it as it stands, and the server's
// ingestOffline reads source verification and the snapshot digest straight out
// of it, holding only the task metadata against its own catalog.
//
// So a record that began as an unverified working tree could finish as
// "verified" with a snapshot digest attached, and nothing between the spool
// directory and durable history would disagree: the start record sitting
// beside it on disk said otherwise the whole time and nobody read it. The
// ordinary version is not an attack at all, it is a caller that reuses its
// entry variable and a run whose metadata silently changes shape between the
// two writes.
//
// The comparison is field by field rather than a whole-struct equality so the
// refusal can name what moved, which is the difference between an operator
// reading "terminal record disagrees" and one reading "source verification".
func checkFinishMatchesStart(started, finishing Entry) error {
	for _, field := range []struct{ name, frozen, given string }{
		{"id", started.ID, finishing.ID},
		{"task", started.Task, finishing.Task},
		{"catalog digest", started.CatalogDigest, finishing.CatalogDigest},
		{"tier", started.Tier, finishing.Tier},
		{"scope", started.Scope, finishing.Scope},
		{"source repository", started.Source.Repository, finishing.Source.Repository},
		{"source branch", started.Source.Branch, finishing.Source.Branch},
		{"source commit", started.Source.CommitSHA, finishing.Source.CommitSHA},
		{"source verification", started.Source.Verification, finishing.Source.Verification},
		{"source snapshot digest", started.Source.SnapshotSHA256, finishing.Source.SnapshotSHA256},
	} {
		if field.frozen != field.given {
			return fmt.Errorf("%w: %s", errFinishDisagrees, field.name)
		}
	}
	if started.SchemaVersion != finishing.SchemaVersion {
		return fmt.Errorf("%w: schema version", errFinishDisagrees)
	}
	if started.DeadlineSeconds != finishing.DeadlineSeconds {
		return fmt.Errorf("%w: deadline", errFinishDisagrees)
	}
	if !started.StartedAt.Equal(finishing.StartedAt) {
		return fmt.Errorf("%w: start stamp", errFinishDisagrees)
	}
	if !slices.Equal(started.Args, finishing.Args) {
		return fmt.Errorf("%w: arguments", errFinishDisagrees)
	}
	return nil
}
