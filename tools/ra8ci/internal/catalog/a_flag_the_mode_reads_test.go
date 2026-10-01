// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// modeStep builds a reviewed shell step dispatching scripts/ci.sh with argv.
func modeStep(argv ...string) Step {
	return Step{
		Name:    "ci",
		Program: DispatchShell,
		Args:    append([]string{"scripts/ci.sh"}, argv...),
	}
}

func TestAFastFlagBesideANamedGateIsRefused(t *testing.T) {
	err := ValidateStepDispatch(modeStep("--gate", "misra", "--fast"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a --fast a single-gate run never reads was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "single-gate") {
		t.Fatalf("the refusal does not name the mode that ignores the flag: %v", err)
	}
	if !strings.Contains(err.Error(), "--fast") {
		t.Fatalf("the refusal does not name the flag: %v", err)
	}
}

func TestARebuildFlagBesideANamedGateIsRefused(t *testing.T) {
	if err := ValidateStepDispatch(modeStep("--gate", "misra", "--rebuild")); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a --rebuild that rebuilds nothing on the single-gate path was admitted: %v", err)
	}
}

func TestANativeFlagBesideANamedGateIsRefused(t *testing.T) {
	if err := ValidateStepDispatch(modeStep("--gate", "misra", "--native")); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a --native the single-gate branch exits before reading was admitted: %v", err)
	}
}

func TestTheEqualsSpellingOfAGateSelectsTheSameMode(t *testing.T) {
	if err := ValidateStepDispatch(modeStep("--gate=misra", "--fast")); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--gate=value did not select the single-gate mode: %v", err)
	}
}

func TestARebuildBesideANativeSuiteIsRefused(t *testing.T) {
	err := ValidateStepDispatch(modeStep("--native", "--rebuild"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a --rebuild the native branch never reads was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "native suite") {
		t.Fatalf("the refusal does not name the native mode: %v", err)
	}
}

func TestAFastFlagBesideANativeSuiteIsAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(modeStep("--native", "--fast")); err != nil {
		t.Fatalf("the native branch is handed fast and reads it, so this must stand: %v", err)
	}
}

func TestAContainerisedNamedGateFallingIntoTheNativeSuiteIsRefused(t *testing.T) {
	err := ValidateStepDispatch(modeStep("--native", "--gate", "misra", "--container"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a named gate the native branch drops on the floor was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "native suite despite a named gate") {
		t.Fatalf("the refusal does not name the mode that drops the gate: %v", err)
	}
}

func TestAContainerisedNamedGateIsAdmittedOnItsOwn(t *testing.T) {
	if err := ValidateStepDispatch(modeStep("--gate", "misra", "--container")); err != nil {
		t.Fatalf("the supported toolchain-image path was refused: %v", err)
	}
}

func TestARebuildOnTheHostPathIsAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(modeStep("--rebuild", "--fast")); err != nil {
		t.Fatalf("host mode reads every flag it is given and must refuse none here: %v", err)
	}
}

func TestAPlainGateStepIsAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(modeStep("--gate", "misra")); err != nil {
		t.Fatalf("the shipped shape of every ci.sh step was refused: %v", err)
	}
}

func TestAContainerWithoutAGateKeepsTheCombinationRefusal(t *testing.T) {
	err := ValidateStepDispatch(modeStep("--container", "--fast"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--container without a gate must still be refused: %v", err)
	}
	if !strings.Contains(err.Error(), "needs a gate name") && !strings.Contains(err.Error(), "--gate") {
		t.Fatalf("the combination door's message did not survive: %v", err)
	}
	if strings.Contains(err.Error(), "unread in that mode") {
		t.Fatalf("this door answered an argv the combination door owns: %v", err)
	}
}

func TestAReportingModeKeepsItsOwnRefusal(t *testing.T) {
	err := ValidateStepDispatch(modeStep("--list-gates", "--fast"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a reporting mode must still be refused: %v", err)
	}
	if strings.Contains(err.Error(), "unread in that mode") {
		t.Fatalf("this door answered an argv the reporting-mode door owns: %v", err)
	}
}

func TestAnUnstatedScriptIsAdmittedOnItsPathAlone(t *testing.T) {
	step := Step{
		Name:    "other",
		Program: DispatchShell,
		Args:    []string{"scripts/checks/format_tree.sh", "--fast"},
	}
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("a script this seam does not state was judged by ci.sh's modes: %v", err)
	}
}

func TestEveryIgnoredOptionIsOneItsScriptParses(t *testing.T) {
	for script, rules := range scriptModesIgnoringOptions {
		if !ReviewedScriptStatesItsOptions(script) {
			t.Fatalf("%s states modes but no option contract", script)
		}
		options := reviewedScriptOptions[script]
		for _, mode := range rules.modes {
			if mode.name == "" || mode.because == "" || len(mode.ignores) == 0 {
				t.Fatalf("%s states an empty mode %q", script, mode.name)
			}
			for _, option := range mode.ignores {
				if !statesExactly(options.valueless, option) && !statesExactly(options.takingTheNextArgument, option) &&
					!statesExactly(options.takingAnEqualsValue, option) {
					t.Fatalf("%s mode %q ignores %q, which its own parser never accepts", script, mode.name, option)
				}
			}
		}
	}
}

func TestScriptModeIgnoringAnswersOnlyForStatedModes(t *testing.T) {
	if _, stated := ScriptModeIgnoring("scripts/ci.sh", "single-gate"); !stated {
		t.Fatal("the single-gate mode is stated and was not reported")
	}
	if _, stated := ScriptModeIgnoring("scripts/ci.sh", "host"); stated {
		t.Fatal("host mode reads every flag and must state no ignored options")
	}
	if _, stated := ScriptModeIgnoring("scripts/checks/format_tree.sh", "single-gate"); stated {
		t.Fatal("an unstated script reported a mode")
	}
}

func TestTheShippedCatalogNamesNoOptionItsModeIgnores(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
