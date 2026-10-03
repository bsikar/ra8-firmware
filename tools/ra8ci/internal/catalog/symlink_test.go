// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"os"
	"runtime"
	"syscall"
	"testing"
)

const errorPrivilegeNotHeld syscall.Errno = 1314

func symlinkTest(t *testing.T, target, link string) {
	t.Helper()
	if err := os.Symlink(target, link); err != nil {
		if runtime.GOOS == "windows" && errors.Is(err, errorPrivilegeNotHeld) {
			t.Skip("os.Symlink returned ERROR_PRIVILEGE_NOT_HELD; this non-admin Windows account lacks symlink privilege")
		}
		t.Fatalf("os.Symlink(%q, %q): %v", target, link, err)
	}
}
