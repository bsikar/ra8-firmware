// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
)

// The repository and branch a local run names are filed in the same kind of
// text column as its task name and step keys (local_runs.repository is text
// CHECK length BETWEEN 1 AND 512, local_runs.branch text NOT NULL DEFAULT ”,
// 0005_offline_sync.sql), and validateLocalRun bounded both of them by LENGTH
// alone. So a NUL, a newline, an escape sequence, or bytes that are not valid
// UTF-8 passed the gate and failed at the INSERT instead, which turns a
// malformed name into a lost upload reported as an unavailable store rather
// than as the invalid record it is.
//
// They are also the two strings an operator matches history by, and the
// repository is what runs_repository_created_idx is read through, so a newline
// there reads as two repositories in anything line-oriented.
func TestARepositoryATextColumnCannotHoldIsRefused(t *testing.T) {
	for _, c := range []struct {
		name  string
		value string
	}{
		{"a NUL", "bsikar/ra8\x00firmware"},
		{"a newline", "bsikar/ra8\nfirmware"},
		{"an escape sequence", "bsikar/\x1b[31mra8-firmware"},
		{"a delete", "bsikar/ra8\x7ffirmware"},
		{"a C1 control", "bsikar/ra8\u0085firmware"},
		{"invalid UTF-8", "bsikar/" + string([]byte{0xff, 0xfe})},
	} {
		run := localRun()
		run.Repository = c.value
		if err := validateLocalRun(run); err == nil {
			t.Fatalf("repository carrying %s was accepted", c.name)
		} else if !errors.Is(err, ErrInvalid) {
			t.Fatalf("repository refusal for %s does not travel as invalid: %v", c.name, err)
		}
	}
}

func TestABranchATextColumnCannotHoldIsRefused(t *testing.T) {
	for _, c := range []struct {
		name  string
		value string
	}{
		{"a NUL", "offline\x00test"},
		{"a newline", "offline\ntest"},
		{"an escape sequence", "\x1b[2Joffline-test"},
		{"invalid UTF-8", "offline" + string([]byte{0xc3, 0x28})},
	} {
		run := localRun()
		run.Branch = c.value
		if err := validateLocalRun(run); err == nil {
			t.Fatalf("branch carrying %s was accepted", c.name)
		} else if !errors.Is(err, ErrInvalid) {
			t.Fatalf("branch refusal for %s does not travel as invalid: %v", c.name, err)
		}
	}
}

// The branch column defaults to the empty string, so a run that names no
// branch is ordinary and stays accepted.
func TestALocalRunNamingNoBranchIsAccepted(t *testing.T) {
	run := localRun()
	run.Branch = ""
	if err := validateLocalRun(run); err != nil {
		t.Fatalf("a local run naming no branch was refused: %v", err)
	}
}

// The rule states what the column requires, not the catalog's alphabet: a
// repository or branch is free to carry printable text beyond ASCII.
func TestPrintableTextBeyondASCIIStaysAcceptable(t *testing.T) {
	run := localRun()
	run.Repository = "bsikar/ra8-firmware-vérification"
	run.Branch = "fonctionnalité/écran"
	if err := validateLocalRun(run); err != nil {
		t.Fatalf("a repository and branch in printable text were refused: %v", err)
	}
}

// The length bounds the columns already stated are kept, not replaced.
func TestTheLengthBoundsAreStillEnforced(t *testing.T) {
	run := localRun()
	run.Repository = strings.Repeat("a", 513)
	if err := validateLocalRun(run); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a repository past the column length was accepted: %v", err)
	}
	run = localRun()
	run.Branch = strings.Repeat("b", 513)
	if err := validateLocalRun(run); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a branch past the column length was accepted: %v", err)
	}
}
