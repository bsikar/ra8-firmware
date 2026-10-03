// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const programFixtureExtension = ".cmd"

func writeProgram(t *testing.T, path, body string) string {
	t.Helper()
	if filepath.Ext(path) != programFixtureExtension {
		path = strings.TrimSuffix(path, filepath.Ext(path)) + programFixtureExtension
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatalf("make program directory: %v", err)
	}
	contents := "@echo off\r\nexit /b 0\r\n"
	if strings.HasPrefix(body, "touch ") {
		witness := strings.TrimPrefix(body, "touch ")
		contents = "@echo ran> \"" + witness + "\"\r\nexit /b 0\r\n"
	}
	if err := os.WriteFile(path, []byte(contents), 0o600); err != nil {
		t.Fatalf("write program: %v", err)
	}
	return path
}
