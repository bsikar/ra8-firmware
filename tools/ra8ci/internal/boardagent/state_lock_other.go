// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//go:build !linux

package boardagent

import (
	"sync"
)

var stateLocks sync.Map

// Non-Linux builds retain in-process safety for tests and portable tooling.
// The physical board-agent mode is Linux-only because cross-process locking
// and supported board transports are provided by the Linux service target.
func acquireStateLock(path string) (func(), error) {
	lock, _ := stateLocks.LoadOrStore(path, &sync.Mutex{})
	mutex := lock.(*sync.Mutex)
	mutex.Lock()
	return mutex.Unlock, nil
}
