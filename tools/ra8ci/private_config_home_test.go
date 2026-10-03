// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"testing"
)

// privateConfigHome points board lease handling at a temporary configuration
// home, so a test never touches the real user configuration directory.
func privateConfigHome(t *testing.T) string {
	t.Helper()
	home := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", home)
	return home
}
