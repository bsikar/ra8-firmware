// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"path/filepath"
	"strings"
)

// inWalkScope reports whether a file found by walking a directory is one this
// gate reads.
//
// The gate has two scopes for the same rule. The derived scope (--all) reads
// every first-party file with a known text extension AND every extensionless
// file whose first line names a shell or python interpreter, because a hook
// or a helper script carries prose that the rule applies to exactly as a .sh
// would; the selftest pins that second set, requiring at least seven of them
// and scripts/git/commit-msg among them. The walk, which is what a developer
// runs over a subtree and what a hook runs over the tree it is installed in,
// read extensions alone, so those same scripts were invisible to it.
//
// Two scans of one rule that disagree about what is in scope is the whole
// problem: a non-ASCII dash added to scripts/git/commit-msg passed a local
// `ascii --check scripts/` cleanly and failed the CI scan that reads the
// derived set, and the developer's own scan was the one that could have
// caught it. A file named directly on the command line was always read
// whatever its extension, so this only ever bit the walk.
//
// The shebang is read from the file itself, so an extensionless file that is
// not a script (a LICENSE, a fixture, a binary without a suffix) stays out of
// scope exactly as it is under --all.
func inWalkScope(path string) bool {
	if textExtensions[strings.ToLower(filepath.Ext(path))] {
		return true
	}
	return filepath.Ext(path) == "" && hasShellOrPythonShebang(path)
}
