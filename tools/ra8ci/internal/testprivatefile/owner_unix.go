//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testprivatefile

import "os"

// ownerOnly keeps a directory traversable by its owner (0700) and a file
// readable and writable by its owner (0600). A directory at 0600 loses its
// search bit, so nothing inside it can be opened.
func ownerOnly(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	if info.IsDir() {
		return os.Chmod(path, 0o700)
	}
	return os.Chmod(path, 0o600)
}

func denyDirectoryRead(path string) error   { return os.Chmod(path, 0o000) }
func denyDirectoryCreate(path string) error { return os.Chmod(path, 0o500) }
func restoreDirectory(path string) error    { return os.Chmod(path, 0o700) }
