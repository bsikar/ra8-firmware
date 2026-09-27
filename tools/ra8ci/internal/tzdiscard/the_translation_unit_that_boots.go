// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	"regexp"
	"strings"
)

// entryPoint matches a mention of either TrustZone boot entry point taking no
// arguments. Deliberately unanchored: the old test demanded the declarator sit
// at the start of a line with "void" immediately before it, so a definition
// carrying an attribute, a section marker or its return type on the line above
// -- all ordinary in boot code -- did not read as a boot translation unit at
// all, and rule B, the wide "any discarded ra8_* result" rule, silently
// switched off for exactly the file it exists to watch.
var entryPoint = regexp.MustCompile(`\b(?:SystemInit|ra8_trustzone_init)\s*\(\s*void\s*\)`)

// definesBootEntry reports whether this translation unit DEFINES a boot entry
// point rather than merely naming one.
//
// The distinction is the whole point. A vector table or a caller that
// forward-declares the entry point ends its declarator at a semicolon and
// emits nothing; treating that as a boot TU widens rule B over a file that
// never boots anything. Following each mention to its body is what separates
// the two, and it is the same question answered in the other direction for the
// escape above: a decorated definition is still a definition.
func definesBootEntry(text string) bool {
	for _, match := range entryPoint.FindAllStringIndex(text, -1) {
		if precededByIdentifierRune(text, match[0]) {
			continue
		}
		if bodyOpensAt(text, match[1]) {
			return true
		}
	}
	return false
}

// precededByIdentifierRune reports whether the name is welded to something
// longer, so a call to ra8_trustzone_init_late() is not read as the entry
// point. The word boundary alone cannot see this: an underscore is a word
// character, so \b never fires between "my_SystemInit" and "SystemInit".
func precededByIdentifierRune(text string, start int) bool {
	if start == 0 {
		return false
	}
	previous := text[start-1]
	return previous == '_' || previous >= 'a' && previous <= 'z' ||
		previous >= 'A' && previous <= 'Z' || previous >= '0' && previous <= '9'
}

// bodyOpensAt reports whether the next meaningful character after the
// declarator opens a body. Whitespace and both comment forms are skipped, so a
// definition annotated between its parameter list and its brace still counts;
// anything else, a semicolon above all, means that mention was a declaration
// and the scan reads on to the next one.
func bodyOpensAt(text string, index int) bool {
	for index < len(text) {
		switch {
		case text[index] == ' ' || text[index] == '\t' || text[index] == '\n' || text[index] == '\r':
			index++
		case strings.HasPrefix(text[index:], "//"):
			end := strings.IndexByte(text[index:], '\n')
			if end < 0 {
				return false
			}
			index += end + 1
		case strings.HasPrefix(text[index:], "/*"):
			end := strings.Index(text[index+2:], "*/")
			if end < 0 {
				return false
			}
			index += end + 4
		default:
			return text[index] == '{'
		}
	}
	return false
}
