// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//go:build !windows

package hilspec

import (
	"os"
	"testing"
)

func symlinkOrSkip(t *testing.T, target, link string) error {
	t.Helper()
	return os.Symlink(target, link)
}
