// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strings"
)

// The shell mirror of checkToolProgramExists, and the door that was missing
// from this branch while six others judged what a step hands a script.
//
// The tool branch settles WHICH program a step names before it judges a single
// argument: checkToolProgramExists refuses an ra8ci: name no runner implements,
// because a name outside the reviewed eighteen fails on every runner as a
// missing tool. The shell branch had no such door. ValidScriptPath judges only
// SHAPE: relative, slash separated, no traversal, no shell metacharacter, .sh,
// inside the checkout. Every one of those holds for scripts/checks/typo.sh, for
// tools/ci/gate.sh, and for a path naming a script that was renamed a year ago.
// Such a step was admitted, digested, shipped under a reviewed digest, and the
// mistake surfaced only on a runner, where bash answers a path it cannot open
// with "No such file or directory" and exit 127, having run nothing, once per
// dispatch, forever, on every runner that takes the task.
//
// That is strictly worse than the tool case it mirrors. A withdrawn tool at
// least leaves a name a reader can search for; a script path is free text that
// looks right until somebody opens the checkout at that revision.
//
// RESTATED, NOT READ OFF DISK, for the same reason every other table on this
// seam is restated: catalog review admits a manifest, and the checkout it is
// admitted against is not the checkout it will run in. Walking the filesystem
// here would judge the reviewer's working tree, which is exactly the fact that
// does not travel with the digest. What travels is the list, and the list is
// the review decision.
//
// DELIBERATELY A CLOSED LIST OF TWO, not a root prefix and not the 135 .sh
// files the repository ships. Those 135 include gate internals, fixtures and
// libraries meant to be sourced rather than dispatched, and a rule admitting
// scripts/ wholesale would let a reviewed step dispatch any of them. The
// dispatch surface is a review decision the same way the tool list is: adding a
// script here is two edits, this list and the task using it, in the change that
// reviews them together. TestTheShippedCatalogDispatchesOnlyReviewedScripts
// holds the list to what the catalog actually names, so the pair cannot drift.
var dispatchableScripts = []string{
	// The gate driver. Carries 64 of the 66 shell steps in the shipped
	// catalog; its argument contract is stated in reviewedScriptOptions.
	"scripts/ci.sh",
	// The formatter. Dispatched with no arguments, so no option contract is
	// stated for it and the option doors admit it on its path alone.
	"scripts/checks/format_tree.sh",
}

// ReviewedScriptPaths returns every script a reviewed step may dispatch.
func ReviewedScriptPaths() []string {
	return append([]string(nil), dispatchableScripts...)
}

// IsReviewedScriptPath reports whether a path is one review has admitted as a
// dispatch target.
func IsReviewedScriptPath(path string) bool {
	for _, candidate := range dispatchableScripts {
		if path == candidate {
			return true
		}
	}
	return false
}

// checkTheScriptIsOneTheCheckoutShips refuses a reviewed step dispatching a
// script outside the reviewed list. It runs before the option doors because a
// script nothing ships has no argument contract to judge: refusing its argv
// first would name the wrong mistake.
//
// Like checkToolProgramExists, this is an ADMISSION rule applied where a
// manifest is read, not a re-check against a task already persisted under a
// reviewed digest. A task admitted while its script existed keeps running if
// the script is later removed, and the removal is what has to re-review it.
func checkTheScriptIsOneTheCheckoutShips(step Step) error {
	script := step.Args[0]
	if IsReviewedScriptPath(script) {
		return nil
	}
	return fmt.Errorf("%w: step %q dispatches %q, which is not a reviewed script; %s dispatches %s, and a path outside them answers %q and exit 127 on every runner, having run nothing",
		ErrInvalidCatalog, step.Name, script, DispatchShell,
		strings.Join(dispatchableScripts, " and "), "No such file or directory")
}
