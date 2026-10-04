//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import "os"

func restrictFile(file *os.File) error {
	if file == nil {
		return os.ErrInvalid
	}
	return file.Chmod(0o600)
}
