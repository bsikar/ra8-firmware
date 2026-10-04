//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testprivatefile

import "os"

func ownerOnly(path string) error { return os.Chmod(path, 0o600) }

func denyDirectoryRead(path string) error   { return os.Chmod(path, 0o000) }
func denyDirectoryCreate(path string) error { return os.Chmod(path, 0o500) }
func restoreDirectory(path string) error    { return os.Chmod(path, 0o700) }
