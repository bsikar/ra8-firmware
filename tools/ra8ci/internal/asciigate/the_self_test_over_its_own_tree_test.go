// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"bytes"
	"context"
	"io"
	"path/filepath"
	"strings"
	"testing"
)

// repositoryRoot is the tree this package ships in, four levels above the
// package directory a test runs from.
func repositoryRoot() string { return filepath.Join("..", "..", "..", "..") }

// The self-test is the gate's claim that it still works on the repository it
// ships in, so it has to hold against that repository and not only against
// fixtures. It was asserting a hardcoded count of extensionless scripts, which
// the tree stopped satisfying, so the gate reported itself broken over a tree
// it was scanning correctly.
func TestTheSelfTestHoldsOverTheRepositoryItShipsIn(t *testing.T) {
	var out, errors bytes.Buffer
	code := Run(context.Background(), repositoryRoot(), []string{"--selftest"}, &out, &errors)
	if code != 0 {
		t.Fatalf("the gate calls itself broken over its own tree: code=%d stderr=%q", code, errors.String())
	}
	if !strings.Contains(out.String(), "selftest passed") {
		t.Fatalf("a passing self-test said nothing: %q", out.String())
	}
	if errors.Len() != 0 {
		t.Fatalf("a passing self-test complained: %q", errors.String())
	}
}

// Every git hook is an extensionless shell script, the one shape the scope
// admits on its first line rather than its name. Each one has to be in the
// derived scope, since a hook the gate never reads is a hook that can carry
// anything.
func TestEveryHookScriptIsInTheDerivedScope(t *testing.T) {
	root := repositoryRoot()
	targets, err := derivedTargets(context.Background(), root)
	if err != nil {
		t.Fatalf("the scope could not be derived: %v", err)
	}
	scoped := make(map[string]bool, len(targets))
	for _, target := range targets {
		scoped[filepath.ToSlash(target)] = true
	}
	if !scoped["scripts/git/commit-msg"] {
		t.Fatal("the commit-msg hook is outside the scope this gate reads")
	}
}

// An incomplete invocation is refused without a crash even when only one of
// the two writers is missing: the announcement goes to the stderr it does
// have, rather than to the stdout it does not.
func TestRunWithoutAPlaceToReport(t *testing.T) {
	var errors bytes.Buffer
	var stdout io.Writer
	if code := Run(context.Background(), repositoryRoot(), nil, stdout, &errors); code != 2 {
		t.Fatalf("an invocation with nowhere to report was not refused: %d", code)
	}
	if !strings.Contains(errors.String(), "invalid input") {
		t.Fatalf("the refusal did not reach the stderr it was given: %q", errors.String())
	}
}
