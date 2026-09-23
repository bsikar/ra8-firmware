//go:build !linux && !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"io"
	"time"
)

func runCommand(context.Context, string, []string, string, []string, io.Writer, io.Writer, time.Duration) (commandResult, error) {
	return commandResult{ExitCode: -1}, ErrUnsupportedOS
}
