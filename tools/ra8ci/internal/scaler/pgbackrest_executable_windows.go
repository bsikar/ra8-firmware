//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"path/filepath"
	"strings"
)

func pgBackRestExecutable(path string, info os.FileInfo) bool {
	if info == nil || !info.Mode().IsRegular() {
		return false
	}
	pathExtension := filepath.Ext(path)
	if pathExtension == "" {
		return false
	}
	pathExtension = strings.ToLower(pathExtension)
	pathExtensions := os.Getenv("PATHEXT")
	if pathExtensions == "" {
		pathExtensions = ".COM;.EXE;.BAT;.CMD"
	}
	for _, extension := range strings.Split(pathExtensions, ";") {
		if strings.ToLower(strings.TrimSpace(extension)) == pathExtension {
			return true
		}
	}
	return false
}

// Windows does not support flushing a directory handle with File.Sync. The
// temporary file is flushed before rename, which is the supported durability
// boundary on this platform.
func syncBackupAttestationDirectory(_ string) error { return nil }
