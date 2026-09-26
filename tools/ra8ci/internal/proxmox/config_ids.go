// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// The two VM ID lists in Config say opposite things about the same number.
// AllowedVMIDs names disposable guests: IDs a pass may observe, stop and
// destroy. TemplateVMIDs names reviewed sources: IDs that must stay
// templates and are never a clone target. checkedIDs already refuses a
// number repeated inside one list; nothing refused a number that appeared in
// both, and every rule downstream assumes it cannot.
//
// What an overlap does, in the code as it stands. List treats a member of
// the allowlist as a disposable guest and refuses the WHOLE listing when one
// turns out to be a template ("allowed VM ID %d is a template"), so declaring
// a reviewed template disposable stops every sweep on the node rather than
// skipping one ID. Destroy takes any allowlisted ID through
// validateIdentity, and the refusal arrives late, from inspect, as "target
// became a template", which describes a race that did not happen instead of
// a configuration that cannot be right. Clone already refuses a spec whose
// source and target are the same number, but that is one call site defending
// itself, not the invariant.
//
// So this is a construction-time refusal, next to the per-list one: a
// contradictory configuration is refused once, at the point where it is
// declared, rather than by each path that later relies on it.
func checkDisjointIDs(allowed, templates map[int]struct{}) error {
	var shared []int
	for id := range templates {
		if _, both := allowed[id]; both {
			shared = append(shared, id)
		}
	}
	if len(shared) == 0 {
		return nil
	}
	sort.Ints(shared)
	names := make([]string, 0, len(shared))
	for _, id := range shared {
		names = append(names, strconv.Itoa(id))
	}
	return fmt.Errorf("%w: VM ID %s is declared both a reviewed template and a disposable guest",
		ErrInvalid, strings.Join(names, ", "))
}
