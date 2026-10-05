//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"errors"
	"testing"
)

// On Unix, EvalSymlinks(".") can remain relative while the temp candidate is
// absolute, so filepath.Rel cannot compare them. Windows resolves the relative
// root to a drive path, and may classify a candidate as outside instead.
func TestIsWithinRefusesARootAndCandidateItCannotRelate(t *testing.T) {
	within, err := isWithin(".", t.TempDir())
	if within || !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("within = %v, error = %v", within, err)
	}
}
