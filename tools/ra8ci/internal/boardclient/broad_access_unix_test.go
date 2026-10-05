//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import "os"

func grantBroadAccess(path string) error { return os.Chmod(path, 0o644) }
