//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

// Windows does not support flushing directory handles. The file is flushed
// before rename; controller deployments use the Linux implementation.
func syncBoardStateDirectory(_ string) error { return nil }
