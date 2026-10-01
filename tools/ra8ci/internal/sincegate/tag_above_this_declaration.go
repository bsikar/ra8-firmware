// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

// presenceLookback is how many lines above a public declaration the presence
// check reads while looking for that declaration's @since tag.
const presenceLookback = 30

// commentAbove returns the lines the presence check may read when it judges the
// public declaration at index.
//
// The lookback exists because a Doxygen block sits above the declaration it
// documents and 30 lines is generous enough for a full block. It is not a
// licence to read a DIFFERENT declaration's block. A window that runs back past
// an earlier public declaration reaches that declaration's @since, and the
// undocumented function below it then passes on the tag written for the one
// above. Two public functions with a single documented block between them is
// the ordinary shape of a header, so nothing exotic is needed to hit it.
//
// The far end already binds the tag to the block it belongs to:
// scripts/checks/doxy_functions.py asks whether "@since" is in the block
// attached to this function, not whether it is somewhere overhead. This side
// has no block parser, so it uses the next best boundary, the previous public
// declaration, and reads only what sits below it.
func commentAbove(lines []string, index int) []string {
	start := index - presenceLookback
	if start < 0 {
		start = 0
	}
	for back := index - 1; back >= start; back-- {
		if publicDecl.MatchString(lines[back]) {
			start = back + 1
			break
		}
	}
	return lines[start:index]
}
