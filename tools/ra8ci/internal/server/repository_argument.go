// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"strings"
	"unicode"
	"unicode/utf8"
)

// maxRepositoryLength bounds the repository text a caller may state. It is the
// length the slow report has always held its query parameter to; the rule is
// stated here so every handler that carries a caller-stated repository into an
// authorization decision reads one definition of it.
const maxRepositoryLength = 512

// usableRepository reports whether a caller-stated repository may be used at
// all. It is asked BEFORE the authorization, because a denial writes the
// repository to the audit trail as its target, and a denial is recorded before
// the request has been understood: whatever reaches that column is text the
// caller chose. Bounding it here refuses the unusable request outright rather
// than trimming attacker text at the record.
//
// Three things are refused: no repository at all, more text than a repository
// name can be, and anything that is not readable as one line. Control
// characters and invalid UTF-8 are refused because an audit record is read
// back as lines, and a target carrying its own newlines can be made to look
// like a record nobody wrote.
//
// It says nothing about whether the repository EXISTS or whether the caller
// may touch it. That remains the authorizer's answer, from the peer
// certificate and the grant behind it, never from the request body.
func usableRepository(repository string) bool {
	if repository == "" || len(repository) > maxRepositoryLength {
		return false
	}
	if !utf8.ValidString(repository) {
		return false
	}
	return !strings.ContainsFunc(repository, unicode.IsControl)
}
