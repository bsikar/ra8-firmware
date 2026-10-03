//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	errors "errors"
	os "os"
	filepath "path/filepath"
	testing "testing"
)

func TestCleanEnvironmentRefusesAPathEntryItCannotResolve(t *testing.T) {
	root := t.TempDir()
	t.Setenv("PATH", occupiedPath(t))
	env, err := cleanEnvironment(root)
	if !errors.Is(err, ErrUnsafeEnvironment) || env != nil {
		t.Fatalf("env = %v, error = %v", env, err)
	}
}

// A failure that is not "it does not exist yet" is handed back rather than
// walked past: a directory the runner cannot enter is a real answer about the
// path, and treating it as absent would let the walk invent a location.
func TestResolvePathHandsBackAFailureThatIsNotAbsence(t *testing.T) {
	root := t.TempDir()
	sealed := filepath.Join(root, "sealed")
	if err := os.Mkdir(sealed, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o700) })
	if _, err := os.Stat(filepath.Join(sealed, "tool")); err == nil || errors.Is(err, os.ErrNotExist) {
		t.Skip("this box enters a mode 0o000 directory")
	}
	if _, err := resolvePath(filepath.Join(sealed, "tool")); err == nil {
		t.Fatal("a sealed directory resolved")
	} else if errors.Is(err, os.ErrNotExist) {
		t.Fatalf("err = %v, want the access failure rather than absence", err)
	}
}

// isWithin's other half: a candidate that cannot be resolved is an unsafe
// environment, not a quiet "outside".
func TestIsWithinRefusesACandidateItCannotResolve(t *testing.T) {
	root := t.TempDir()
	sealed := filepath.Join(root, "sealed")
	if err := os.Mkdir(sealed, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o700) })
	if _, err := os.Stat(filepath.Join(sealed, "tool")); err == nil || errors.Is(err, os.ErrNotExist) {
		t.Skip("this box enters a mode 0o000 directory")
	}
	within, err := isWithin(root, filepath.Join(sealed, "tool"))
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("err = %v, want ErrUnsafeEnvironment", err)
	}
	if within {
		t.Fatal("an unresolvable candidate was judged inside")
	}
}
