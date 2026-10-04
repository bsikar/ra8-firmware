//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testprivatefile

import "os"

func unreadable(path string) error {
	return os.Chmod(path, 0)
}
