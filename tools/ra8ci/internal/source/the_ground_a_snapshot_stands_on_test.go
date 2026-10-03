// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package source

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// A snapshot is an identity claim about a checkout, so the two things it
// stands on are the ground the path is resolved against and the path each
// step of the walk composes. Neither is stated by the caller: the first comes
// from the process's own working directory, and the second is built inside
// the recursion. What is held here is that both are refused when they cannot
// be trusted, rather than resolved into something else.

func TestARelativeCheckoutWithNoGroundUnderItIsRefused(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows does not allow removing the process working directory")
	}
	// A working directory that has been removed under the process. Every
	// relative path is now meaningless, and resolving one anyway would
	// identify some other tree.
	gone := t.TempDir()
	t.Chdir(gone)
	if err := os.RemoveAll(gone); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Getwd(); err == nil {
		t.Skip("this filesystem still answers for a removed working directory")
	}

	_, err := Snapshot(context.Background(), "checkout")
	if err == nil {
		t.Fatal("a relative checkout was resolved against a working directory that is gone")
	}
	if !errors.Is(err, ErrGit) {
		t.Fatalf("refusal should be a git refusal: %v", err)
	}
	if !strings.Contains(err.Error(), "root") {
		t.Fatalf("refusal should name the root it could not resolve: %v", err)
	}
}

func TestARelativeCheckoutOnGroundThatHoldsIsStillRead(t *testing.T) {
	gitBinary(t)
	parent := t.TempDir()
	newRepository(t, filepath.Join(parent, "checkout"), "first.txt")
	t.Chdir(parent)

	// The same shape of call as the refusal above, so that refusal cannot be
	// read as this package declining relative roots in general.
	result, err := Snapshot(context.Background(), "checkout")
	if err != nil {
		t.Fatalf("a relative checkout on ground that holds was refused: %v", err)
	}
	if !validObjectID(result.RootCommit) || !validSHA256(result.Digest) {
		t.Fatalf("snapshot answered commit %q digest %q", result.RootCommit, result.Digest)
	}
}

func TestTheWalkJudgesThePathItComposesRatherThanTheOneItWasHanded(t *testing.T) {
	// Each gitlink path is already judged as it is parsed, so the composed
	// child path is a second reading of the same rule, one level down. It is
	// what keeps the recursion honest about where it is: a walk that trusted
	// the composition would descend on a path leading out of the checkout
	// while every piece it was handed looked ordinary.
	gitPath := stubbedGit(t, []byte("160000 commit "+theCommit+"\tlibs/dep\x00"))
	root := t.TempDir()

	entries := make([]Entry, 0, 2)
	err := inspectTree(context.Background(), gitPath, root, root, "..", "", 0, &entries)
	if !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("a child path composed outside the checkout was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "libs/dep") {
		t.Fatalf("refusal should name the composed path: %v", err)
	}

	// The refusal is the composition's doing: the same tree under a relative
	// this walk could actually reach records the child and keeps going.
	entries = entries[:0]
	if err := inspectTree(context.Background(), gitPath, root, root, "vendor", "", 0, &entries); err == nil {
		t.Fatal("expected the stub to be followed into the child it names")
	}
	if len(entries) == 0 || entries[0].Path != "vendor" {
		t.Fatalf("the tree it was handed was not recorded before the descent: %+v", entries)
	}
}
