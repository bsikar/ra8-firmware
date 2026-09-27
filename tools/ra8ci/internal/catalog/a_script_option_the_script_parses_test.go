// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// unstatedScript dispatches a real reviewed script whose argument contract is
// deliberately NOT stated, so a test about some other rule can hand a step any
// argv without this door having an opinion about it.
const unstatedScript = "scripts/checks/format_tree.sh"

func scriptStep(args ...string) Step {
	return Step{Name: "gate", Program: DispatchShell, Args: args}
}

func TestAGateStepNamingAGateIsAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate", "format")); err != nil {
		t.Fatalf("the shape every CI workflow step uses must be admitted: %v", err)
	}
}

func TestAGateStepPassingAnOptionTheScriptDoesNotParseIsRefused(t *testing.T) {
	err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gates", "format"))
	if err == nil {
		t.Fatal("an option the script answers with usage must be refused at review")
	}
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the refusal must be an invalid-catalog error: %v", err)
	}
	if !strings.Contains(err.Error(), "--gate <value>") {
		t.Fatalf("the refusal must name what the script does parse: %v", err)
	}
}

func TestASingleDashSpellingOfAScriptOptionIsRefused(t *testing.T) {
	// The tool doors read -flag and --flag alike because Go's flag package
	// does. A shell case arm does not, so this door must not either.
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "-fast")); err == nil {
		t.Fatal("ci.sh matches exact spellings and reaches its unknown-flag arm on -fast")
	}
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--fast")); err != nil {
		t.Fatalf("the spelling the script does parse must be admitted: %v", err)
	}
}

func TestAGateOptionWithNoNameIsRefused(t *testing.T) {
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate")); err == nil {
		t.Fatal("a gate option with nothing after it must not reach a runner")
	}
}

func TestAGateOptionTakingAnOptionAsItsNameIsRefused(t *testing.T) {
	err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate", "--fast"))
	if err == nil {
		t.Fatal("the script takes the next element literally, so this names a gate nobody chose")
	}
	if !strings.Contains(err.Error(), "--fast") {
		t.Fatalf("the refusal must name the value that would have been read as a gate: %v", err)
	}
}

func TestAnEmptyGateValueIsRefused(t *testing.T) {
	err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate="))
	if err == nil {
		t.Fatal("--gate= sets an empty gate and silently runs the whole suite")
	}
	if !strings.Contains(err.Error(), "whole suite") {
		t.Fatalf("the refusal must say what the step would actually have run: %v", err)
	}
}

func TestTheEqualsFormOfAGateIsAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate=format")); err != nil {
		t.Fatalf("the script has a --gate=* arm: %v", err)
	}
}

func TestAnEqualsFormTheScriptHasNoArmForIsRefused(t *testing.T) {
	// --selftest-abort shifts and takes the next element; there is no
	// --selftest-abort=* arm, so the =form reaches the unknown-flag arm.
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--selftest-abort=hang")); err == nil {
		t.Fatal("an =form the script has no arm for must be refused")
	}
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--selftest-abort", "hang")); err != nil {
		t.Fatalf("the spelling the script does parse must be admitted: %v", err)
	}
}

func TestABareWordIsRefusedWhereTheScriptTakesNoPositional(t *testing.T) {
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "format")); err == nil {
		t.Fatal("ci.sh has no positional arm; a bare word is an unknown flag to it")
	}
}

func TestSeveralOptionsTheScriptParsesAreAdmittedTogether(t *testing.T) {
	if err := ValidateStepDispatch(scriptStep("scripts/ci.sh", "--gate", "format", "--container")); err != nil {
		t.Fatalf("the containerised single-gate shape is one the script states: %v", err)
	}
}

func TestAScriptStatingNoOptionsIsAdmittedOnItsPathAlone(t *testing.T) {
	if ReviewedScriptStatesItsOptions(unstatedScript) {
		t.Fatal("this test is about a script with no stated contract")
	}
	if err := ValidateStepDispatch(scriptStep(unstatedScript, "--check")); err != nil {
		t.Fatalf("a script with no stated contract keeps the rule it had: %v", err)
	}
}

func TestEveryStatedEqualsSpellingAlsoTakesAValue(t *testing.T) {
	for script, stated := range reviewedScriptOptions {
		for _, spelling := range stated.takingAnEqualsValue {
			if !statesExactly(stated.takingTheNextArgument, spelling) {
				t.Fatalf("%s: %q states an =form but is not a value-taking option", script, spelling)
			}
		}
		for _, spelling := range stated.valueless {
			if statesExactly(stated.takingTheNextArgument, spelling) {
				t.Fatalf("%s: %q is stated both as valueless and as value-taking", script, spelling)
			}
		}
	}
}

func TestTheShippedCatalogPassesTheScriptOptionDoor(t *testing.T) {
	catalog, err := Load()
	if err != nil {
		t.Fatalf("the shipped catalog must load: %v", err)
	}
	judged := 0
	for _, name := range catalog.Names() {
		task, _ := catalog.Task(name)
		for _, step := range task.Steps {
			if step.Program != DispatchShell || len(step.Args) == 0 || !ReviewedScriptStatesItsOptions(step.Args[0]) {
				continue
			}
			judged++
			if err := checkScriptOptionsAreOnesTheScriptParses(step); err != nil {
				t.Fatalf("shipped step %q of task %q: %v", step.Name, name, err)
			}
		}
	}
	if judged == 0 {
		t.Fatal("no shipped step reaches this door, so it proves nothing")
	}
}
