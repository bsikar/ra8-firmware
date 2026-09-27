// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import "sort"

// stepsInRunnerOrder hands a job's steps back in the order the runner ran
// them, which is the order every ordering rule in this package already
// believes it is reading.
//
// scanJob's central finding, "'a' ended and 'b' began before it", is a claim
// about two ADJACENT steps, and adjacency came from nothing but the position
// of the two objects in the JSON array. The runner states that order itself:
// every step carries a number, 1 upward, and this package did not decode the
// field at all. So the scanner leaned on the serialization and ignored the
// one field in the payload that says what ran when.
//
// Both directions of that land on a host that is fine. A list handed back in
// any other order makes ordinary steps look overlapped, and the scan exits 1
// naming a runner for a clock fault it does not have. A list something
// reserialized by timestamp makes a real overlap unreachable: the step that
// began too early is no longer behind the step it collided with, and the
// report ends on "every step on every runner is time-ordered" over the exact
// fault #509 recorded. The second is the worse one, because a truncated run
// list at least still counted what it read, and this one reads everything and
// judges it in the wrong sequence.
//
// The reorder is only taken when the numbers can carry it: every step
// numbered at least 1, and no number used twice. Anything else (a payload
// carrying no numbers, a job whose steps repeat one) is handed back exactly
// as it arrived, because half an ordering key is a worse witness than the
// sequence the API gave. The caller's slice is never reordered in place; a
// job is read by more than one rule here and each of them is entitled to the
// same list.
func stepsInRunnerOrder(input []step) []step {
	if len(input) < 2 {
		return input
	}
	seen := make(map[int]bool, len(input))
	for _, item := range input {
		if item.Number < 1 || seen[item.Number] {
			return input
		}
		seen[item.Number] = true
	}
	ordered := make([]step, len(input))
	copy(ordered, input)
	sort.SliceStable(ordered, func(i, j int) bool { return ordered[i].Number < ordered[j].Number })
	return ordered
}
