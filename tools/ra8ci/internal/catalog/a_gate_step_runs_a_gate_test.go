// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func TestAStepNamingTheRegistryDumpIsRefused(t *testing.T) {
	err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--list-gates"))
	if err == nil {
		t.Fatal("a step that dumps the registry and exits 0 would be recorded as a passing gate")
	}
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the refusal must be an invalid-catalog error: %v", err)
	}
	if !strings.Contains(err.Error(), "read nothing") {
		t.Fatalf("the refusal must say what the green verdict would have proved: %v", err)
	}
}

func TestAStepNamingUsageIsRefusedInBothSpellings(t *testing.T) {
	for _, spelling := range []string{"-h", "--help"} {
		if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", spelling)); err == nil {
			t.Fatalf("%s prints usage and exits 0 without running a gate", spelling)
		}
	}
}

func TestAReportingModeIsRefusedBesideTheGateItWouldSkip(t *testing.T) {
	// The arm sits before the single-gate branch, so the gate name is
	// parsed, never used.
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate", "format", "--list-gates")); err == nil {
		t.Fatal("naming a gate does not stop the registry dump from returning first")
	}
}

func TestAnOrdinaryGateStepIsStillAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate", "format", "--container")); err != nil {
		t.Fatalf("a step that actually runs its gate must be admitted: %v", err)
	}
}

func TestTheInternalProbeIsDeliberatelyNotRefusedHere(t *testing.T) {
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--selftest-abort", "hang")); err != nil {
		t.Fatalf("the probe runs the real suite runner and is not a reporting mode: %v", err)
	}
}

func TestAReportingModeSpellingIsReadExactly(t *testing.T) {
	// --list-gates=1 is not an arm of the parser at all, so the option
	// door refuses it first and this one never has to guess.
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--list-gates=1")); err == nil {
		t.Fatal("a spelling the parser has no arm for must still be refused")
	}
}

func TestAScriptStatingNoReportingModesIsAdmitted(t *testing.T) {
	if _, stated := ScriptReportingModes(unstatedScript); stated {
		t.Fatal("this test is about a script with no stated reporting modes")
	}
	if err := ValidateStepDispatch(scriptStep(unstatedScript, "--help")); err != nil {
		t.Fatalf("a script with no stated contract keeps the rule it had: %v", err)
	}
}

func TestEveryStatedReportingModeIsOneItsScriptParses(t *testing.T) {
	for script, modes := range scriptReportingModes {
		stated, known := reviewedScriptOptions[script]
		if !known {
			t.Fatalf("%s states reporting modes but no argument contract", script)
		}
		for _, mode := range modes {
			if !statesExactly(stated.valueless, mode) {
				t.Fatalf("%s: %q is not a valueless option its parser accepts", script, mode)
			}
		}
	}
}

func TestTheShippedCatalogNamesNoReportingMode(t *testing.T) {
	catalog, err := Load()
	if err != nil {
		t.Fatalf("the shipped catalog must load: %v", err)
	}
	for _, name := range catalog.Names() {
		task, _ := catalog.Task(name)
		for _, step := range task.Steps {
			if err := checkAScriptStepRunsTheWorkItNames(step); err != nil {
				t.Fatalf("shipped step %q of task %q: %v", step.Name, name, err)
			}
		}
	}
}
