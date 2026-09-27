// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import (
	"strings"
	"unicode"
)

// optOutMarker is the per-line opt-out this gate documents, spelled the one way
// the repository's suppression catalog inventories it.
const optOutMarker = "WAVE-OK"

// lineStatesAWaveOptOut reports whether a line actually claims the opt-out.
//
// The gate prints one contract and the suppression catalog knows one marker:
// "WAVE-OK: <reason>". Matching the marker and its colon alone accepts two
// things that are not that contract. A bare "WAVE-OK:" silences the line while
// promising a reason it never gives, and the reason is the whole point of the
// escape hatch, since it is all a later reader has to judge the suppression by.
// A marker welded onto the end of a longer token, "NOT-WAVE-OK:" or
// "xWAVE-OK:", is a line writing about the annotation rather than claiming it,
// and it silenced the line just the same.
//
// So the marker has to begin a token of its own, and something other than
// whitespace has to follow its colon on the same line. Whitespace between the
// marker and the colon stays allowed: prose around it may have been wrapped,
// and this gate has always read it that way.
func lineStatesAWaveOptOut(line string) bool {
	for index := 0; index < len(line); {
		offset := strings.Index(line[index:], optOutMarker)
		if offset < 0 {
			return false
		}
		start := index + offset
		index = start + len(optOutMarker)
		if attachedToAToken(line[:start]) {
			continue
		}
		rest := strings.TrimLeftFunc(line[index:], unicode.IsSpace)
		if !strings.HasPrefix(rest, ":") {
			continue
		}
		if strings.TrimSpace(rest[1:]) == "" {
			continue
		}
		return true
	}
	return false
}

// attachedToAToken reports whether the text immediately before the marker ends
// in a character that makes the marker part of a longer word. The hyphen counts
// because the marker carries one itself, so "NOT-WAVE-OK" reads as a single
// term rather than as this annotation.
func attachedToAToken(before string) bool {
	if before == "" {
		return false
	}
	runes := []rune(before)
	previous := runes[len(runes)-1]
	return previous == '_' || previous == '-' || unicode.IsLetter(previous) || unicode.IsDigit(previous)
}
