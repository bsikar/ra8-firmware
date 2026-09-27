// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package unsafeinstall

import "strings"

// The override's name is assembled from pieces for the same reason the flag
// itself is: this file is first-party and inside the gate's own scan scope, so
// a spelled-out literal here would report the detector as a finding.
var overrideKey = "break" + "-system-" + "packages"

// falsyValues are the values that turn the override OFF. A line that pins the
// override to one of these is the opposite of what this gate bans, so it is
// not a finding.
var falsyValues = []string{"0", "false", "no", "off", "n"}

// statesTheOverride reports whether the line turns the PEP 668 protection off.
//
// pip takes the same instruction three ways and they are the same decision:
// the command-line flag, the PIP_-prefixed environment variable of the same
// name that every pip run reads, and the same name as a key written into
// pip.conf by `pip config set`. Only the flag was ever detected, so a
// Dockerfile ENV line or a CI env block disabled the protection for every pip
// call in its scope and passed this gate untouched.
//
// Matching is done on a lowercased copy with underscores folded to hyphens,
// which is what makes the environment variable's spelling the same string as
// the flag's and the config key's. The names themselves are never written out
// here, so that reading this file does not report the detector as a finding.
func statesTheOverride(line string) bool {
	normalized := strings.ReplaceAll(strings.ToLower(line), "_", "-")
	index := strings.Index(normalized, overrideKey)
	if index < 0 {
		return false
	}
	rest := strings.TrimLeft(normalized[index+len(overrideKey):], " \t=:\"'`")
	value := rest
	if cut := strings.IndexFunc(value, func(r rune) bool {
		return (r < 'a' || r > 'z') && (r < '0' || r > '9')
	}); cut >= 0 {
		value = value[:cut]
	}
	for _, falsy := range falsyValues {
		if value == falsy {
			return false
		}
	}
	return true
}
