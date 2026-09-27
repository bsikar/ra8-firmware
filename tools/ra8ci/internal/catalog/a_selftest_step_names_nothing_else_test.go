// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"testing"
)

func TestASelftestStepNamingNothingElseIsAdmitted(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if err := ValidateStepDispatch(optionStep(program, "--selftest")); err != nil {
			t.Errorf("%s must admit its own self-test step: %v", program, err)
		}
	}
}

func TestASelftestStepCarryingAModeBesideItIsRefused(t *testing.T) {
	err := ValidateStepDispatch(optionStep("ra8ci:ascii", "--selftest", "--all"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog for a combined self test, got %v", err)
	}
}

func TestASelftestStepCarryingAFileBesideItIsRefused(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:no-null", "--selftest", "drivers/ra8_batt.c")); err == nil {
		t.Fatal("a file beside the self test is answered with usage on every runner")
	}
}

func TestTheSelftestDoorReadsTheSingleDashAndValuedForms(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:ascii", "-selftest", "--check")); err == nil {
		t.Fatal("-selftest names the same option")
	}
	if err := ValidateStepDispatch(optionStep("ra8ci:runner-clock", "--selftest=true", "--ci-scan")); err == nil {
		t.Fatal("--selftest=true names the same option")
	}
}

func TestWaveReferencesRefusesACombinedSelftestItWouldRunAnyway(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:wave-references", "--selftest", "--selftest")); err == nil {
		t.Fatal("wave-references runs the self test whatever else argv carries, so review must refuse the pair")
	}
}

func TestAScanStepWithSeveralOptionsIsStillAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(optionStep("ra8ci:ascii", "--check", "--all")); err != nil {
		t.Fatalf("only the self test is exclusive: %v", err)
	}
}

func TestTheSelftestDoorSaysNothingAboutShellSteps(t *testing.T) {
	step := Step{Name: "gate", Program: DispatchShell, Args: []string{unstatedScript, "--selftest", "--all"}}
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("a reviewed script owns its own options: %v", err)
	}
}

func TestTheEmbeddedCatalogPairsEverySelftestWithItsOwnStep(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must be admitted: %v", err)
	}
}
