// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package privatefile validates that a file is readable only by its owner.
package privatefile

// Check reports whether path names a regular file protected for its owner.
func Check(path string) error { return check(path) }
