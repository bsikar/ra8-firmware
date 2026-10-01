// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//go:build linux

package boardagent

import (
	"os"
	"syscall"
)

// acquireStateLock serializes high-water reads and writes across independent
// board-agent processes. The state directory is private, but O_NOFOLLOW and
// file identity checks still reject a substituted lock path.
func acquireStateLock(path string) (func(), error) {
	fd, err := syscall.Open(path, syscall.O_CREAT|syscall.O_RDWR|syscall.O_CLOEXEC|syscall.O_NOFOLLOW, 0o600)
	if err != nil {
		return nil, ErrUnsafeState
	}
	file := os.NewFile(uintptr(fd), path)
	info, err := file.Stat()
	linkInfo, linkErr := os.Lstat(path)
	if err != nil || linkErr != nil || !info.Mode().IsRegular() ||
		info.Mode().Perm()&0o077 != 0 || !os.SameFile(info, linkInfo) {
		_ = file.Close()
		return nil, ErrUnsafeState
	}
	if err := syscall.Flock(fd, syscall.LOCK_EX); err != nil {
		_ = file.Close()
		return nil, ErrUnsafeState
	}
	return func() {
		_ = syscall.Flock(fd, syscall.LOCK_UN)
		_ = file.Close()
	}, nil
}
