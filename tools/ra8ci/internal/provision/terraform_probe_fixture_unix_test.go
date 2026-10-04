//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"os"
	"path/filepath"
	"testing"
)

func installTerraformProbe(_ *testing.T, directory, body string) (string, error) {
	path := filepath.Join(directory, "terraform")
	return path, os.WriteFile(path, []byte(body), 0o755)
}
