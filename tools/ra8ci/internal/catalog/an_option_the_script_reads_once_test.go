// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func repeatStep(argv ...string) Step {
	return Step{
		Name:    "gate",
		Program: DispatchShell,
		Args:    append([]string{"scripts/ci.sh"}, argv...),
	}
}

func TestAnOptionNamedTwiceIsRefused(t *testing.T) {
	err := checkNoScriptOptionIsNamedTwice(repeatStep("--gate", "misra", "--gate", "format"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a second --gate silently replaces the first: %v", err)
	}
}

func TestTheRefusalNamesBothValuesAndWhichOneSurvives(t *testing.T) {
	err := checkNoScriptOptionIsNamedTwice(repeatStep("--gate", "misra", "--gate", "format"))
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{"misra", "format", "--gate"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q so a reader can find both elements: %v", want, err)
		}
	}
}

func TestTheTwoSpellingsOfOneOptionAreTheSameOption(t *testing.T) {
	for _, argv := range [][]string{
		{"--gate", "misra", "--gate=format"},
		{"--gate=misra", "--gate", "format"},
		{"--gate=misra", "--gate=format"},
	} {
		if err := checkNoScriptOptionIsNamedTwice(repeatStep(argv...)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%v sets one variable through two arms: %v", argv, err)
		}
	}
}

func TestTheProbeModeIsReadOnceToo(t *testing.T) {
	err := checkNoScriptOptionIsNamedTwice(repeatStep("--selftest-abort", "hang", "--selftest-abort", "fail"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the abort probe assigns one variable as well: %v", err)
	}
}

// A switch carries no value, so a repeat discards no intention. Refusing it
// would be the door judging tidiness rather than meaning.
func TestARepeatedSwitchIsNotRefusedHere(t *testing.T) {
	for _, argv := range [][]string{
		{"--fast", "--fast"},
		{"--native", "--rebuild", "--native"},
	} {
		if err := checkNoScriptOptionIsNamedTwice(repeatStep(argv...)); err != nil {
			t.Fatalf("%v selects the same mode twice and drops nothing: %v", argv, err)
		}
	}
}

func TestAnOptionNamedOnceIsAdmitted(t *testing.T) {
	for _, argv := range [][]string{
		{"--gate", "misra"},
		{"--gate=misra", "--container"},
		{"--fast", "--selftest-abort", "hang"},
		{},
	} {
		if err := checkNoScriptOptionIsNamedTwice(repeatStep(argv...)); err != nil {
			t.Fatalf("%v names nothing twice: %v", argv, err)
		}
	}
}

// A value that happens to repeat an option's own name is still a value: the
// parser shifted past it. Only an element the loop reads as an option counts.
func TestAValueIsNotCountedAsASecondOption(t *testing.T) {
	if err := checkNoScriptOptionIsNamedTwice(repeatStep("--selftest-abort", "--gate", "--gate", "misra")); err != nil {
		t.Fatalf("the first --gate is the probe mode's value, however badly chosen: %v", err)
	}
}

func TestATrailingOptionWithNoValueIsLeftToTheOptionDoor(t *testing.T) {
	step := repeatStep("--gate", "misra", "--gate")
	if err := checkNoScriptOptionIsNamedTwice(step); err != nil {
		t.Fatalf("a missing value is the option door's refusal, not this one: %v", err)
	}
	if err := checkScriptOptionsAreOnesTheScriptParses(step); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("and that door must still make it: %v", err)
	}
}

func TestAnUnstatedScriptCountsNothingHere(t *testing.T) {
	step := Step{Name: "gate", Program: DispatchShell, Args: []string{"scripts/checks/format_tree.sh", "--gate", "a", "--gate", "b"}}
	if err := checkNoScriptOptionIsNamedTwice(step); err != nil {
		t.Fatalf("no contract is stated for this script: %v", err)
	}
}

func TestAToolStepIsNotJudgedHere(t *testing.T) {
	step := Step{Name: "scan", Program: ToolProgramPrefix + "ascii", Args: []string{"--all", "--all"}}
	if err := checkNoScriptOptionIsNamedTwice(step); err != nil {
		t.Fatalf("this door reads the shell branch only: %v", err)
	}
}

func TestEveryOptionThisDoorCountsCarriesAValue(t *testing.T) {
	for script, stated := range reviewedScriptOptions {
		for _, option := range stated.valueless {
			if ScriptOptionCarriesAValue(script, option) {
				t.Fatalf("%s %s is a switch and must not be counted", script, option)
			}
		}
		for _, option := range append(append([]string(nil), stated.takingTheNextArgument...), stated.takingAnEqualsValue...) {
			if !ScriptOptionCarriesAValue(script, option) {
				t.Fatalf("%s %s carries a value and must be counted", script, option)
			}
		}
	}
	if ScriptOptionCarriesAValue("scripts/checks/format_tree.sh", "--gate") {
		t.Fatal("an unstated script states nothing")
	}
}

func TestTheShippedCatalogNamesNoScriptOptionTwice(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
