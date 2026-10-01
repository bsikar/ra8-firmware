// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// valueStep builds a reviewed shell step dispatching scripts/ci.sh with argv.
func valueStep(argv ...string) Step {
	return Step{
		Name:    "probe",
		Program: DispatchShell,
		Args:    append([]string{"scripts/ci.sh"}, argv...),
	}
}

func TestAnAbortProbeModeTheScriptDrivesIsAdmitted(t *testing.T) {
	for _, mode := range []string{"hang", "destroy", "fail"} {
		if err := ValidateStepDispatch(valueStep("--selftest-abort", mode)); err != nil {
			t.Fatalf("%s is one of the probe's own registries: %v", mode, err)
		}
	}
}

func TestAnAbortProbeModeTheScriptNamesNowhereIsRefused(t *testing.T) {
	err := ValidateStepDispatch(valueStep("--selftest-abort", "crash"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a probe mode with no registry behind it was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "crash") || !strings.Contains(err.Error(), "hang, destroy, fail") {
		t.Fatalf("the refusal names neither the value nor what the script takes: %v", err)
	}
}

func TestAnAbortProbeModeIsReadExactly(t *testing.T) {
	for _, mode := range []string{"Hang", "hang ", "hangs", ""} {
		if err := ValidateStepDispatch(valueStep("--selftest-abort", mode)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("a shell case arm is exact, so %q must be refused: %v", mode, err)
		}
	}
}

func TestAMissingProbeModeStaysTheOptionDoorsRefusal(t *testing.T) {
	err := ValidateStepDispatch(valueStep("--selftest-abort"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--selftest-abort with nothing after it must be refused: %v", err)
	}
	if strings.Contains(err.Error(), "no arm of its own case") {
		t.Fatalf("this door answered an argv the option door owns: %v", err)
	}
}

func TestAGateNameIsNotJudgedHere(t *testing.T) {
	if err := ValidateStepDispatch(valueStep("--gate", "a-gate-no-registry-has")); err != nil {
		t.Fatalf("the gate registry is deliberately not restated on this seam: %v", err)
	}
}

func TestAValuelessOptionIsNotReadAsAValueTaker(t *testing.T) {
	if err := ValidateStepDispatch(valueStep("--fast")); err != nil {
		t.Fatalf("a valueless switch has no value to judge: %v", err)
	}
}

func TestAnUnstatedScriptHasNoValueContract(t *testing.T) {
	step := Step{
		Name:    "other",
		Program: DispatchShell,
		Args:    []string{"scripts/checks/format_tree.sh", "--selftest-abort", "crash"},
	}
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("a script this seam does not state was judged by ci.sh's values: %v", err)
	}
}

func TestScriptOptionValuesAnswersOnlyForJudgedOptions(t *testing.T) {
	if _, stated := ScriptOptionValues("scripts/ci.sh", "--selftest-abort"); !stated {
		t.Fatal("the probe mode is judged and was not reported")
	}
	if _, stated := ScriptOptionValues("scripts/ci.sh", "--gate"); stated {
		t.Fatal("the gate registry is not restated here and must report nothing")
	}
	if _, stated := ScriptOptionValues("scripts/checks/format_tree.sh", "--selftest-abort"); stated {
		t.Fatal("an unstated script reported a value contract")
	}
}

func TestEveryJudgedScriptOptionIsOneItsScriptTakesAValueFor(t *testing.T) {
	for script, options := range reviewedScriptOptionValues {
		if !ReviewedScriptStatesItsOptions(script) {
			t.Fatalf("%s states option values but no option contract", script)
		}
		stated := reviewedScriptOptions[script]
		for option, values := range options {
			if !statesExactly(stated.takingTheNextArgument, option) {
				t.Fatalf("%s judges values for %q, which its parser takes no value for", script, option)
			}
			if len(values) == 0 {
				t.Fatalf("%s states an empty value set for %q", script, option)
			}
			for _, value := range values {
				if value == "" || strings.HasPrefix(value, "-") {
					t.Fatalf("%s states %q as a value of %q; a value is not a flag", script, value, option)
				}
			}
		}
	}
}

func TestTheShippedCatalogHandsNoScriptAValueItRefuses(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
