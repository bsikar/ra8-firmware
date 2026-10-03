// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//go:build unix

package hilspec

import (
	"os"
	"testing"
)

// A file the reader cannot open is reported as the open failure, not as an
// invalid manifest: nothing was parsed, so nothing can be called invalid.
func TestAManifestThatWillNotOpenIsRefused(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root: a sealed file still opens")
	}
	root, relative := plantManifest(t, "HIL_MODE=alive\n", 0o000)
	if _, err := Load(root, relative); err == nil {
		t.Fatal("a sealed manifest was read")
	}
}
