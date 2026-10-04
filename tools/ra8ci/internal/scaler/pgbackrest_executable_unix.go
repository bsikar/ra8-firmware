//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import "os"

func pgBackRestExecutable(_ string, info os.FileInfo) bool {
	return info.Mode().Perm()&0o111 != 0
}

func syncBackupAttestationDirectory(directory string) error {
	handle, err := os.Open(directory)
	if err != nil {
		return err
	}
	defer handle.Close()
	return handle.Sync()
}
