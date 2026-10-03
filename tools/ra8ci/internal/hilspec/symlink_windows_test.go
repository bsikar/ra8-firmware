// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"errors"
	"os"
	"syscall"
	"testing"
)

func symlinkOrSkip(t *testing.T, target, link string) error {
	t.Helper()
	if err := os.Symlink(target, link); err != nil {
		if errors.Is(err, syscall.ERROR_PRIVILEGE_NOT_HELD) {
			t.Skipf("symlink fixture requires Windows SeCreateSymbolicLinkPrivilege: %v", err)
		}
		return err
	}
	return nil
}
