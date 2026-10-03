//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import (
	"fmt"
	"os"
)

func check(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat private file %s: %w", path, err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("private file %s is not a regular file", path)
	}
	if mode := info.Mode().Perm(); mode&0o077 != 0 {
		return fmt.Errorf("private file %s mode %04o grants access beyond its owner", path, mode)
	}
	return nil
}
