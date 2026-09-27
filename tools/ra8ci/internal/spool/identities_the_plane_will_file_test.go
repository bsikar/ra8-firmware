// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"
)

// filable builds the source identity an ordinary local checkout reports.
func filable(repository, branch string) SourceIdentity {
	return SourceIdentity{Repository: repository, Branch: branch,
		CommitSHA: strings.Repeat("a", 40), Verification: "unverified"}
}

func TestIdentitiesThePlaneWillFileAcceptsEveryOrdinaryShape(t *testing.T) {
	for _, c := range []struct {
		name   string
		task   string
		source SourceIdentity
	}{
		{"an ordinary run", "format-check", filable("bsikar/ra8-firmware", "ra8ci/dev")},
		{"no branch stated", "format-check", filable("bsikar/ra8-firmware", "")},
		{"a branch with slashes and dots", "format-check", filable("bsikar/ra8-firmware", "release/v1.2.x")},
		{"text outside ASCII", "format-check", filable("bsikar/ra8-firmware", "feature/µ-controller")},
		{"a repository at the ceiling", "format-check", filable(strings.Repeat("r", maxFilableSourceBytes), "ra8ci/dev")},
		{"a task name at the ceiling", strings.Repeat("t", maxFilableTaskBytes), filable("bsikar/ra8-firmware", "ra8ci/dev")},
	} {
		if err := checkIdentitiesAreOnesThePlaneWillFile(c.task, c.source); err != nil {
			t.Fatalf("%s was refused: %v", c.name, err)
		}
	}
}

func TestIdentitiesAboveTheCeilingsAreRefused(t *testing.T) {
	ordinary := filable("bsikar/ra8-firmware", "ra8ci/dev")
	for _, c := range []struct {
		name   string
		task   string
		source SourceIdentity
	}{
		{"an empty task name", "", ordinary},
		{"a task name one byte over", strings.Repeat("t", maxFilableTaskBytes+1), ordinary},
		{"a repository one byte over", "format-check", filable(strings.Repeat("r", maxFilableSourceBytes+1), "ra8ci/dev")},
		{"a branch one byte over", "format-check", filable("bsikar/ra8-firmware", strings.Repeat("b", maxFilableSourceBytes+1))},
	} {
		if err := checkIdentitiesAreOnesThePlaneWillFile(c.task, c.source); !errors.Is(err, errUnfilableIdentity) {
			t.Fatalf("%s was accepted: %v", c.name, err)
		}
	}
}

func TestATaskNameTheplaneWouldTrimIsRefused(t *testing.T) {
	err := checkIdentitiesAreOnesThePlaneWillFile(" format-check", filable("bsikar/ra8-firmware", "ra8ci/dev"))
	if !errors.Is(err, errUnfilableIdentity) {
		t.Fatalf("a task name with surrounding whitespace was accepted: %v", err)
	}
}

func TestIdentitiesATextColumnCannotCarryAreRefused(t *testing.T) {
	for _, c := range []struct {
		name   string
		task   string
		source SourceIdentity
	}{
		{"a NUL in the task name", "format\x00check", filable("bsikar/ra8-firmware", "ra8ci/dev")},
		{"a newline in the branch", "format-check", filable("bsikar/ra8-firmware", "ra8ci/dev\nother")},
		{"an escape sequence in the repository", "format-check", filable("bsikar/\x1b[31mra8", "ra8ci/dev")},
		{"a C1 control in the branch", "format-check", filable("bsikar/ra8-firmware", "ra8ci/\u009bdev")},
		{"bytes that are not UTF-8", "format-check", filable("bsikar/\xff\xfe", "ra8ci/dev")},
	} {
		if err := checkIdentitiesAreOnesThePlaneWillFile(c.task, c.source); !errors.Is(err, errUnfilableIdentity) {
			t.Fatalf("%s was accepted: %v", c.name, err)
		}
	}
}

// The rule mirrors store.namesATextColumnCanHold, so ordinary punctuation and
// text outside ASCII stay acceptable: only the control ranges are refused.
func TestTextThePlaneCanFileRefusesOnlyTheControlRanges(t *testing.T) {
	for _, value := range []string{"", "ra8ci/dev", "µ-controller", "日本語", "release/v1.2.x-rc1"} {
		if !textThePlaneCanFile(value) {
			t.Fatalf("%q was refused", value)
		}
	}
	for _, value := range []string{"\x00", "\n", "\t", "\x7f", "\u0085"} {
		if textThePlaneCanFile(value) {
			t.Fatalf("%q was accepted", value)
		}
	}
}

func TestBeginRefusesAnIdentityThePlaneWillNotFile(t *testing.T) {
	spool, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	metadata := Metadata{Source: filable("bsikar/ra8-firmware", "ra8ci/dev\nsecond"),
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 600}
	if _, err := spool.BeginWithMetadata("format-check", strings.Repeat("b", 64), metadata); !errors.Is(err, errUnfilableIdentity) {
		t.Fatalf("an unfilable branch was frozen into a start record: %v", err)
	}
	metadata.Source = filable("bsikar/ra8-firmware", "ra8ci/dev")
	if _, err := spool.BeginWithMetadata(strings.Repeat("t", maxFilableTaskBytes+1), strings.Repeat("b", 64), metadata); !errors.Is(err, errUnfilableIdentity) {
		t.Fatalf("a task name above the ceiling was frozen into a start record: %v", err)
	}
	entry, err := spool.BeginWithMetadata("format-check", strings.Repeat("b", 64), metadata)
	if err != nil {
		t.Fatalf("an ordinary identity was refused: %v", err)
	}
	if entry.Task != "format-check" || entry.Source.Branch != "ra8ci/dev" {
		t.Fatalf("the record does not carry the identity it froze: %+v", entry)
	}
}
