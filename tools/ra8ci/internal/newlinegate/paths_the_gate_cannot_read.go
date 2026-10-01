// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import "fmt"

// unreadable names a path the gate was asked to judge and could not open.
//
// Run's exit codes already state the rule this serves: 0 is clean, 1 is
// findings, and 2 means the scan could not be trusted. The derived sweep is
// held to that twice, by trackedFloor on what git listed and fileFloor on what
// survived the filters, both there because a sweep that collapses to nothing
// otherwise prints a clean line over a tree it never opened.
//
// The explicit path had no such guard, in two places. A named path that could
// not be stat'd was kept as a target whenever its suffix looked like source,
// and the scan loop answered every read error with continue. So a path typed
// wrong, a file renamed since the caller built its list, or a dangling symlink
// inside a named directory came back as "1 file(s) scanned, all end in a
// newline." That is the failure the floors exist to prevent, arriving one file
// at a time and reported as a pass, which is worse than the collapsed sweep
// because the caller asked about that exact file and was told it was fine.
//
// A path that opens and is simply not first-party source, a .md or a .json, is
// still skipped in silence: a caller hands over a list and expects the gate to
// pick its own files out of it. What is refused is only a path the gate cannot
// read at all, and the refusal names it and says why.
func unreadable(root, path string, err error) error {
	return fmt.Errorf("cannot read %s: %w", displayPath(root, path), err)
}
