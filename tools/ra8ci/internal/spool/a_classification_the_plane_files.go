// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
)

// The classifications durable history files a local run under. They are
// column constraints, not this build's opinion: migrations/0005_offline_sync.sql
// declares local_runs.tier CHECK (tier IN ('required', 'optional', 'nightly'))
// and local_runs.scope CHECK (scope IN ('safe-local-read-only',
// 'safe-local-write-working-tree')), and store.validateLocalRun restates both
// before the insert. Restated here rather than imported: internal/store does
// not depend on this package and this package does not depend on it.
var (
	filableTiers  = []string{"required", "optional", "nightly"}
	filableScopes = []string{"safe-local-read-only", "safe-local-write-working-tree"}
)

// errUnfilableClassification names the one thing this rule refuses: a run
// begun under a tier or a scope the plane has no column value for.
var errUnfilableClassification = errors.New("local run classified in a way the plane will not file")

// checkTheClassificationIsOneThePlaneFiles holds the tier and the scope a run
// is begun under to the values the columns that file them accept.
//
// BeginWithMetadata asks whether a classification was STATED (tier != "",
// scope != "") and bounds the deadline to 1..86400, and it has never asked
// whether the two words it was handed are words the plane files. Everything
// else frozen at Begin is now held to the far end's rule: the arguments
// (arguments_the_plane_will_take.go) and the three identities
// (identities_the_plane_will_file.go). The classification was the last field
// on the start record taken on trust.
//
// THIS IS THE FREEZE, and the timing is the whole value. tier and scope are
// written into the start record BEFORE the first command runs, and
// checkFinishMatchesStart holds the terminal record to the start record on
// both fields, so a tier the plane will not file cannot be corrected after
// the fact: the run executes, the record is frozen, and the evidence is
// unuploadable forever. Refused here, the caller is told before the work is
// spent, which is the same argument arguments_the_plane_will_take.go makes
// and the reason that door sits at Begin rather than at Finish.
//
// syncclient.checkUploadedClassificationIsOneThePlaneFiles states the same
// rule on the read-back path, and that is a different door: Pending reads a
// record off disk through json.Unmarshal, so a file written by an older
// build or edited by hand arrives at the sweep having passed nothing here.
// Freeze versus read-back is the cut; neither shadows the other.
//
// DELIBERATELY NOT THE DEADLINE. begin already bounds DeadlineSeconds to
// 1..86400, which is the column's whole CHECK, so restating it here would
// report one record under two errors and leave two places to change if the
// bound ever moves.
//
// DELIBERATELY NOT a claim about whether this tier suits this task. The
// reviewed definition decides that, the catalog holds it, and a spooled
// record carries only the digest of the catalog it came from. Membership in
// the column's value set is the whole question here.
func checkTheClassificationIsOneThePlaneFiles(tier, scope string) error {
	if !aValueTheColumnHolds(tier, filableTiers) {
		return fmt.Errorf("%w: tier %q is none of %v", errUnfilableClassification, tier, filableTiers)
	}
	if !aValueTheColumnHolds(scope, filableScopes) {
		return fmt.Errorf("%w: scope %q is none of %v", errUnfilableClassification, scope, filableScopes)
	}
	return nil
}

func aValueTheColumnHolds(value string, allowed []string) bool {
	for _, candidate := range allowed {
		if value == candidate {
			return true
		}
	}
	return false
}
