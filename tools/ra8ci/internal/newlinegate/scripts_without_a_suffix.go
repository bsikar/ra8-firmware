// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"os"
	"path/filepath"
	"strings"
)

// scriptFloor is the number of extensionless scripts the selftest expects to
// find in the derived scope. The checkout holds five, all under scripts/git;
// the floor sits below that so a hook being renamed does not fail the gate,
// while a scope that loses the whole set does.
const scriptFloor = 3

// isScriptWithoutSuffix reports whether an extensionless file is a shell or
// python script, read from its own first line.
//
// isSource judges a suffix, plus three names spelled out (CMakeLists.txt,
// justfile, Justfile). Every first-party script that carries no suffix at all
// was therefore in neither scope of this gate: scripts/git/commit-msg,
// hook-launcher, post-checkout, post-commit and post-merge are shell scripts
// the repository installs as git hooks, and the final-newline rule applies to
// them exactly as it does to a .sh. They were not being scanned, by --all or
// by a named directory.
//
// The sibling gate settled the same question the other way round in
// asciigate.inWalkScope: its derived scope already read these files and only
// its walk was blind to them, and the reason given there holds here too, that
// one rule with two scopes that disagree is the whole problem. This gate was
// blind in both.
//
// The shebang is read from the file, so an extensionless file that is not a
// script (a LICENSE, a fixture, a binary without a suffix) stays out of scope.
func isScriptWithoutSuffix(path string) bool {
	if filepath.Ext(path) != "" {
		return false
	}
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	defer file.Close()
	var head [200]byte
	read, _ := file.Read(head[:])
	line := string(head[:read])
	if !strings.HasPrefix(line, "#!") {
		return false
	}
	line = strings.ReplaceAll(line[2:], "/usr/bin/env", " ")
	line = strings.ReplaceAll(line, "/", " ")
	for _, word := range strings.Fields(line) {
		switch strings.SplitN(word, "-", 2)[0] {
		case "sh", "bash", "zsh", "dash", "python", "python3":
			return true
		}
	}
	return false
}
