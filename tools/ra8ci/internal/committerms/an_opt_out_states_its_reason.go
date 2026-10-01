// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package committerms

import (
	"strings"
	"unicode"
)

// legacyOptOut is the annotation that silences a paragraph. It is ASCII, so a
// rune slice and a byte length agree on its length.
const legacyOptOut = "LEGACY-OK"

// lineStatesALegacyOptOut reports whether a line carries a complete
// "LEGACY-OK: <reason>" annotation.
//
// The annotation is the widest escape hatch in this gate: one line silences
// every banned term in its whole paragraph. The contract everywhere it is
// described (scripts/git/commit-msg, scripts/checks/check_inclusive_terminology.py,
// and this package's own selftest) is that it states the reason the legacy
// term had to survive, which is what makes it reviewable. A bare "LEGACY-OK:"
// carried none and silenced the paragraph anyway, so the cheapest way past the
// gate was the one that explains nothing.
//
// Two things are therefore required. The reason must be on the annotation's
// own line, because the paragraph is the unit being excused and a reason
// promised on some later line cannot be read back to it. And the annotation
// must stand on its own, not be welded into a longer token: "NOT-LEGACY-OK:"
// and "xLEGACY-OK:" are somebody writing about the annotation, not claiming
// one. Case still does not matter, and the whitespace Git wraps into a message
// may still sit between the annotation and its colon.
func lineStatesALegacyOptOut(line string) bool {
	runes := []rune(line)
	for index := 0; index+len(legacyOptOut) <= len(runes); index++ {
		if !strings.EqualFold(string(runes[index:index+len(legacyOptOut)]), legacyOptOut) {
			continue
		}
		if index > 0 && attachedToAToken(runes[index-1]) {
			continue
		}
		cursor := index + len(legacyOptOut)
		for cursor < len(runes) && unicode.IsSpace(runes[cursor]) {
			cursor++
		}
		if cursor >= len(runes) || runes[cursor] != ':' {
			continue
		}
		if strings.TrimSpace(string(runes[cursor+1:])) != "" {
			return true
		}
	}
	return false
}

// attachedToAToken reports whether a rune binds the annotation to whatever
// precedes it. The hyphen counts because the annotation carries one itself, so
// without it "NOT-LEGACY-OK" reads as an annotation.
func attachedToAToken(value rune) bool {
	return value == '-' || isWord(value)
}
