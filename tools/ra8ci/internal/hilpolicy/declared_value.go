// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import "strings"

// declared_spelling.go closed the door on a HIL_TIMEOUT_S written in a
// spelling this reader does not interpret. It could only close half of it.
// That check runs on the key side of a line that was already split on "=",
// so it is never reached by a line carrying no "=" at all: strings.Cut
// reports hasEquals=false and the scan moves on before the key is ever
// looked at.
//
// A config meant for sourcing states things about a variable without
// assigning to it, and `unset HIL_TIMEOUT_S` is the ordinary one. The bench
// runner declares every knob empty and then sources hil.conf
// (scripts/hil/lib/hil_conf.sh), so that line leaves the shell with no
// declaration and run_local.sh falls back to HIL_TIMEOUT_S:-30. This reader
// skipped the line in silence, so a file reading
//
//	HIL_TIMEOUT_S=180
//	unset HIL_TIMEOUT_S
//
// gave the shell 30 and this reader 180: the two readers disagree about the
// one thing declared_spelling.go says they have to agree about, which is
// whether the app declared a timeout at all. `export HIL_TIMEOUT_S` with no
// value is the same silence from the other direction, and a bare
// `HIL_TIMEOUT_S` line is a typo that currently reads as an absence.
//
// So a line that names HIL_TIMEOUT_S and assigns nothing is a statement
// about the declaration this reader cannot interpret, and it is refused with
// the line named rather than skipped, which is how every other refusal in
// this reader already behaves. The boundary is the same one keyNamesTheTimeout
// draws: the token has to stand alone, so `unset HIL_MODE` and a line naming
// RA8_HIL_TIMEOUT_S are still skipped in silence. A line that does carry an
// "=" is not this rule's business, because the spelling door owns it and
// owning it twice would report the wrong reason.

// lineDeclaresTheTimeoutWithoutAValue reports whether a hil.conf line names
// HIL_TIMEOUT_S while assigning nothing to it. The comment stripping and
// trimming happen before this is asked, so line is the statement itself.
func lineDeclaresTheTimeoutWithoutAValue(line string) bool {
	if strings.Contains(line, "=") {
		return false
	}
	return keyNamesTheTimeout(line)
}
