// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package catalog embeds the reviewed task manifest and its digest.
package catalog

import _ "embed"

//go:embed tasks.json
var manifest []byte

//go:embed sha256.txt
var digest []byte

// Manifest returns a copy of the embedded task definitions.
func Manifest() []byte {
	return append([]byte(nil), manifest...)
}

// Digest returns the reviewed digest file's contents.
func Digest() []byte {
	return append([]byte(nil), digest...)
}
