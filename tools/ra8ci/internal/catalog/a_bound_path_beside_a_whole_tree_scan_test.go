// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func TestADeclaredPositionalIsRefusedBesideAWholeTreeScan(t *testing.T) {
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}}, "ra8ci:no-null", "--all")
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
	if !strings.Contains(err.Error(), "path") || !strings.Contains(err.Error(), "--all") {
		t.Fatalf("refusal should name the argument and the flag: %v", err)
	}
}

func TestADeclaredPositionalIsRefusedBesideTheSingleDashSpelling(t *testing.T) {
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}}, "ra8ci:since", "-all")
	if !errors.Is(ValidateReviewedTask(task), ErrInvalidCatalog) {
		t.Fatal("the flag package reads -all and --all identically")
	}
}

func TestADeclaredPositionalIsAdmittedWithoutAWholeTreeScan(t *testing.T) {
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}}, "ra8ci:ascii", "--checkout")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a bound path is the real dispatch when the step names no scan: %v", err)
	}
}

func TestADeclaredFlagIsNotJudgedAgainstAWholeTreeScan(t *testing.T) {
	task := boundTask(t, ArgsSchema{Flags: []string{"check"}}, "ra8ci:ascii", "--all")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a bound flag is an option, not a target: %v", err)
	}
}

func TestAWholeTreeScanOnAScriptStepIsNotJudged(t *testing.T) {
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}},
		"bash", "scripts/checks/format_tree.sh", "--all")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a reviewed script reads its own argv: %v", err)
	}
}

func TestTheShippedCatalogHasNoBoundPathBesideAScan(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
