// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"
)

// repeated builds an argument list of the given length, each element distinct
// so a test that fails names a position rather than a value.
func repeated(count int) []string {
	arguments := make([]string, count)
	for i := range arguments {
		arguments[i] = "--flag=value"
	}
	return arguments
}

func TestArgumentsThePlaneWillFileAcceptsEveryOrdinaryList(t *testing.T) {
	for _, c := range []struct {
		name      string
		arguments []string
	}{
		{"no arguments at all", nil},
		{"an empty list", []string{}},
		{"one positional", []string{"tools/ra8ci"}},
		{"a positional and a flag", []string{"tools/ra8ci", "--jobs=4"}},
		{"an empty element", []string{""}},
		{"a tab inside a value", []string{"a\tb"}},
		{"a newline inside a value", []string{"first\nsecond"}},
		{"text outside ASCII", []string{"µ-controller", "日本語"}},
		{"exactly the ceiling", repeated(maxLocalRunArguments)},
	} {
		if err := checkArgumentsAreOnesThePlaneWillFile(c.arguments); err != nil {
			t.Fatalf("%s was refused: %v", c.name, err)
		}
	}
}

func TestArgumentsAboveTheCeilingAreRefused(t *testing.T) {
	err := checkArgumentsAreOnesThePlaneWillFile(repeated(maxLocalRunArguments + 1))
	if !errors.Is(err, errUnfilableArguments) {
		t.Fatalf("a list one over the ceiling was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "65") {
		t.Fatalf("the refusal does not say how many arrived: %v", err)
	}
}

func TestAnArgumentHoldingANULIsRefused(t *testing.T) {
	err := checkArgumentsAreOnesThePlaneWillFile([]string{"fine", "bad\x00value"})
	if !errors.Is(err, errUnfilableArguments) {
		t.Fatalf("an argument holding a NUL was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "argument 1") {
		t.Fatalf("the refusal does not name which argument: %v", err)
	}
}

func TestAnArgumentThatIsNotUTF8IsRefused(t *testing.T) {
	err := checkArgumentsAreOnesThePlaneWillFile([]string{"ok", "ok", "\xff\xfe"})
	if !errors.Is(err, errUnfilableArguments) {
		t.Fatalf("an argument that is not UTF-8 was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "argument 2") {
		t.Fatalf("the refusal does not name which argument: %v", err)
	}
}

// The ceiling is the plane's, so a list this door accepts is one the plane's
// own count bound accepts too. Pinned directly so the two cannot drift apart
// silently: store.validateLocalRun refuses more than 64.
func TestTheCeilingIsTheOneThePlaneFiles(t *testing.T) {
	if maxLocalRunArguments != 64 {
		t.Fatalf("the ceiling is %d, and the plane files 64", maxLocalRunArguments)
	}
}

func TestBeginRefusesArgumentsThePlaneWillNotFile(t *testing.T) {
	spool, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	metadata := Metadata{
		Source: SourceIdentity{Repository: "bsikar/ra8-firmware",
			Branch:    "ra8ci/dev",
			CommitSHA: strings.Repeat("a", 40), Verification: "unverified"},
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 600,
	}
	metadata.Args = []string{"bad\x00value"}
	if _, err := spool.BeginWithMetadata("format-check", strings.Repeat("b", 64), metadata); !errors.Is(err, errUnfilableArguments) {
		t.Fatalf("an unfilable argument was frozen into a start record: %v", err)
	}
	metadata.Args = repeated(maxLocalRunArguments + 1)
	if _, err := spool.BeginWithMetadata("format-check", strings.Repeat("b", 64), metadata); !errors.Is(err, errUnfilableArguments) {
		t.Fatalf("a list above the ceiling was frozen into a start record: %v", err)
	}
}

func TestBeginStillFreezesAnOrdinaryArgumentList(t *testing.T) {
	spool, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	entry, err := spool.BeginWithMetadata("format-check", strings.Repeat("b", 64), Metadata{
		Source: SourceIdentity{Repository: "bsikar/ra8-firmware",
			Branch:    "ra8ci/dev",
			CommitSHA: strings.Repeat("a", 40), Verification: "unverified"},
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 600,
		Args: []string{"tools/ra8ci", "--jobs=4"},
	})
	if err != nil {
		t.Fatalf("an ordinary argument list was refused: %v", err)
	}
	if len(entry.Args) != 2 || entry.Args[0] != "tools/ra8ci" || entry.Args[1] != "--jobs=4" {
		t.Fatalf("the record does not carry the arguments it froze: %q", entry.Args)
	}
}
