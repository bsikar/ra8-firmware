//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testprivatefile

import "os"

func otherUsersWritable(path string) error { return os.Chmod(path, 0o666) }
