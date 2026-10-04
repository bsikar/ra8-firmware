//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"path/filepath"
	"testing"
)

func installPgBackRestFixture(_ *testing.T, directory, body string, mode os.FileMode) (string, error) {
	path := filepath.Join(directory, "pgbackrest")
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		return "", err
	}
	return path, os.Chmod(path, mode)
}

func rewritePgBackRestFixture(_ *testing.T, path, body string, mode os.FileMode) error {
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		return err
	}
	return os.Chmod(path, mode)
}
