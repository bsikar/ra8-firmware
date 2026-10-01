// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
)

func TestAStatedPrincipalIsAccepted(t *testing.T) {
	for _, principal := range []string{
		"principal",
		"ra8ci-submitter-01",
		"bsikar@tuta.io",
		"spiffe://ra8ci/submitter/bench-3",
	} {
		run := localRun()
		run.PrincipalID = principal
		if err := validateLocalRun(run); err != nil {
			t.Fatalf("principal %q was refused: %v", principal, err)
		}
	}
}

// The principal is the identity every local run is looked up by: the index is
// (principal_id, received_at), the idempotent key is (principal_id,
// local_id), and an operator matches a run by reading it. A spelling that
// cannot be read back, or cannot be written at all, is refused here rather
// than inside the ingest transaction.
func TestAPrincipalHistoryCannotNameIsRefused(t *testing.T) {
	for _, c := range []struct {
		name  string
		value string
	}{
		{"a NUL", "princi\x00pal"},
		{"a newline", "principal\nother-principal"},
		{"a carriage return", "principal\rother"},
		{"a tab", "princi\tpal"},
		{"an escape sequence", "principal\x1b[31m"},
		{"a delete", "princi\x7fpal"},
		{"a C1 control", "principal\u0085other"},
		{"invalid UTF-8", "principal" + string([]byte{0xff, 0xfe})},
	} {
		run := localRun()
		run.PrincipalID = c.value
		err := validateLocalRun(run)
		if err == nil {
			t.Fatalf("a principal carrying %s was accepted", c.name)
		}
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("the refusal for %s does not travel as invalid: %v", c.name, err)
		}
	}
}

// The length bound this rule sits beside is unchanged.
func TestThePrincipalLengthBoundStillHolds(t *testing.T) {
	run := localRun()
	run.PrincipalID = strings.Repeat("p", 256)
	if err := validateLocalRun(run); err != nil {
		t.Fatalf("a principal at the bound was refused: %v", err)
	}
	run.PrincipalID = strings.Repeat("p", 257)
	if err := validateLocalRun(run); err == nil {
		t.Fatal("a principal past the bound was accepted")
	}
	run.PrincipalID = ""
	if err := validateLocalRun(run); err == nil {
		t.Fatal("an empty principal was accepted")
	}
}

// Non-ASCII is not the thing being refused: a principal may be a person's
// name, and the store holds any text a reader can read.
func TestANonASCIIPrincipalIsAccepted(t *testing.T) {
	run := localRun()
	run.PrincipalID = "équipe-vérification"
	if err := validateLocalRun(run); err != nil {
		t.Fatalf("a non-ASCII principal was refused: %v", err)
	}
}
