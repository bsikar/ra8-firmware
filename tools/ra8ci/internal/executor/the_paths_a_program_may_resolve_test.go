// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// resolvePath answers for a path that does not exist yet by resolving the
// deepest part that does and re-joining what is missing, so a step naming an
// output file is judged by where it would land rather than refused.
func TestResolvePathRejoinsEveryMissingComponent(t *testing.T) {
	root := t.TempDir()
	real, err := filepath.EvalSymlinks(root)
	if err != nil {
		t.Fatal(err)
	}
	for name, item := range map[string]struct{ ask, want string }{
		"an existing directory": {root, real},
		"one missing name":      {filepath.Join(root, "out"), filepath.Join(real, "out")},
		"several missing names": {filepath.Join(root, "a", "b", "c"), filepath.Join(real, "a", "b", "c")},
		"a path needing a clean": {
			filepath.Join(root, "a", "..", "b", ".", "c"),
			filepath.Join(real, "b", "c"),
		},
	} {
		got, err := resolvePath(item.ask)
		if err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		if got != item.want {
			t.Errorf("%s: resolvePath(%q) = %q, want %q", name, item.ask, got, item.want)
		}
	}
}

// The checkout is the thing under test, so it may not also supply the tool
// doing the testing. A resolved program inside it is refused by name, and one
// outside it is left alone.
func TestAProgramResolvingIntoTheCheckoutIsRefused(t *testing.T) {
	root := t.TempDir()
	inside := filepath.Join(root, "scripts", "gate.sh")
	if err := os.MkdirAll(filepath.Dir(inside), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(inside, []byte("#!/bin/sh\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	err := checkProgramIsOutsideTheCheckout(root, inside)
	if !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("err = %v, want ErrUnsafeEnvironment", err)
	}
	if !strings.Contains(err.Error(), inside) {
		t.Fatalf("err = %v, want the program named", err)
	}
	outside := filepath.Join(t.TempDir(), "gate.sh")
	if err := os.WriteFile(outside, []byte("#!/bin/sh\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := checkProgramIsOutsideTheCheckout(root, outside); err != nil {
		t.Fatalf("a system tool was refused: %v", err)
	}
}

// A symlink in a directory outside the checkout whose target is inside it is
// the shape the PATH check cannot see, because that check resolves the
// directory and not the entries within it. This is the door that closes it.
func TestASymlinkOutsideTheCheckoutPointingInIsRefused(t *testing.T) {
	root := t.TempDir()
	target := filepath.Join(root, "tool")
	if err := os.WriteFile(target, []byte("#!/bin/sh\n"), 0o700); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(t.TempDir(), "tool")
	symlinkTest(t, target, link)
	if err := checkProgramIsOutsideTheCheckout(root, link); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("err = %v, want ErrUnsafeEnvironment", err)
	}
}

// The check answers on the root it is given, so a root that cannot be resolved
// is handed back rather than read as a program that is safely outside.
func TestTheProgramCheckRefusesARootItCannotResolve(t *testing.T) {
	root := t.TempDir()
	if err := checkProgramIsOutsideTheCheckout(filepath.Join(root, "absent"), root); !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("err = %v, want ErrUnsafeEnvironment", err)
	}
}
