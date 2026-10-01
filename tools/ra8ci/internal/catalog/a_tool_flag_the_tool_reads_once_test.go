// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

const repeatClockProgram = ToolProgramPrefix + "runner-clock"

func repeatClockStep(args ...string) Step {
	return Step{Name: "clock", Program: repeatClockProgram, Args: args}
}

func TestAValueFlagNamedTwiceIsRefused(t *testing.T) {
	for _, args := range [][]string{
		{"--runs", "250", "--runs", "5"},
		{"--runs=250", "--runs=5"},
		{"--runs", "250", "--runs=5"},
		{"-runs=250", "--runs", "5"},
		{"--repo", "bsikar/ra8-firmware", "--hours", "24", "--repo", "bsikar/other"},
	} {
		if err := checkNoToolFlagIsNamedTwice(repeatClockStep(args...), repeatClockProgram); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%v keeps the last and drops the first: %v", args, err)
		}
	}
}

func TestTheRefusalNamesBothSpellings(t *testing.T) {
	err := checkNoToolFlagIsNamedTwice(repeatClockStep("--runs", "250", "--runs", "5"), repeatClockProgram)
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{"--runs", "250", "5"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q so a reader can find both elements: %v", want, err)
		}
	}
}

// A bare switch named twice sets the same flag true twice and discards no
// intention, the same call the shell door makes about a repeated --fast.
func TestABareSwitchNamedTwiceIsNotRefusedHere(t *testing.T) {
	step := Step{Name: "scan", Program: ToolProgramPrefix + "ascii", Args: []string{"--all", "--all"}}
	if err := checkNoToolFlagIsNamedTwice(step, ToolProgramPrefix+"ascii"); err != nil {
		t.Fatalf("--all --all is true and true again: %v", err)
	}
}

// A boolean given an explicit value is a different matter: the second silently
// undoes the first.
func TestASwitchGivenAValueTwiceIsRefused(t *testing.T) {
	step := Step{Name: "scan", Program: ToolProgramPrefix + "ascii", Args: []string{"--all", "--all=false"}}
	if err := checkNoToolFlagIsNamedTwice(step, ToolProgramPrefix+"ascii"); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--all=false undoes the --all before it: %v", err)
	}
}

func TestAFlagNamedOnceIsAdmitted(t *testing.T) {
	for _, args := range [][]string{
		{"--runs", "250"},
		{"--repo", "bsikar/ra8-firmware", "--runs=250", "--hours", "24"},
		{"--ci-scan"},
		{},
	} {
		if err := checkNoToolFlagIsNamedTwice(repeatClockStep(args...), repeatClockProgram); err != nil {
			t.Fatalf("%v names nothing twice: %v", args, err)
		}
	}
}

// The value after a value flag is swallowed, so a value that happens to spell
// the flag's own name is not a second occurrence of it.
func TestAValueIsNotReadAsASecondFlag(t *testing.T) {
	if err := checkNoToolFlagIsNamedTwice(repeatClockStep("--repo", "--runs", "--runs", "5"), repeatClockProgram); err != nil {
		t.Fatalf("the first --runs is the repo value, however badly chosen: %v", err)
	}
}

func TestATargetIsNotReadAsAFlag(t *testing.T) {
	step := Step{Name: "scan", Program: ToolProgramPrefix + "final-newline", Args: []string{"src/a.c", "src/b.c"}}
	if err := checkNoToolFlagIsNamedTwice(step, ToolProgramPrefix+"final-newline"); err != nil {
		t.Fatalf("two targets are not two flags: %v", err)
	}
}

func TestAnUnstatedToolStatesNoContract(t *testing.T) {
	step := Step{Name: "scan", Program: ToolProgramPrefix + "not-a-tool", Args: []string{"--runs", "1", "--runs", "2"}}
	if err := checkNoToolFlagIsNamedTwice(step, ToolProgramPrefix+"not-a-tool"); err != nil {
		t.Fatalf("no flag table is stated for this name: %v", err)
	}
}

func TestEveryFlagTakingAValueIsOneItsToolParses(t *testing.T) {
	for program, names := range toolFlagsTakingAValue {
		parsed, stated := reviewedToolFlags[program]
		if !stated {
			t.Fatalf("%s takes values for a program off the dispatch list", program)
		}
		for _, name := range names {
			found := false
			for _, candidate := range parsed {
				if candidate == name {
					found = true
				}
			}
			if !found {
				t.Fatalf("%s --%s is not a flag that tool parses", program, name)
			}
			if !ToolFlagTakesAValue(program, name) {
				t.Fatalf("%s --%s must report that it carries a value", program, name)
			}
		}
	}
	if ToolFlagTakesAValue(repeatClockProgram, "selftest") {
		t.Fatal("--selftest is a mode switch")
	}
}

// Every flag with a value rule must be one this door knows carries a value,
// or the two tables disagree about the same argv.
func TestEveryJudgedValueFlagIsOneThisDoorKnowsCarriesAValue(t *testing.T) {
	for program, rules := range toolFlagValueRules {
		for name := range rules {
			if !ToolFlagTakesAValue(program, name) {
				t.Fatalf("%s --%s has a value rule but is not listed as taking a value", program, name)
			}
		}
	}
}

func TestTheShippedCatalogNamesNoToolFlagTwice(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
