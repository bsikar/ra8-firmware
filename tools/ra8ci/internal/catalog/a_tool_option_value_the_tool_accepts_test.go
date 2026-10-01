// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

const clockProgram = "ra8ci:runner-clock"

func clockStep(args ...string) Step {
	return Step{Name: "clock-step", Program: clockProgram, Args: args}
}

func TestARunsValueBelowTheFloorIsRefused(t *testing.T) {
	for _, args := range [][]string{{"--runs", "0"}, {"--runs=0"}, {"-runs", "-5"}} {
		err := ValidateStepDispatch(clockStep(args...))
		if err == nil {
			t.Fatalf("%v was admitted; runner-clock answers --runs below 1 with exit 2", args)
		}
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%v: refusal is not an invalid-catalog error: %v", args, err)
		}
	}
}

func TestARunsValueAtTheFloorIsAdmitted(t *testing.T) {
	for _, args := range [][]string{{"--runs", "1"}, {"--runs=40"}, {"-runs=999"}} {
		if err := ValidateStepDispatch(clockStep(args...)); err != nil {
			t.Fatalf("%v was refused: %v", args, err)
		}
	}
}

// runner-clock states a floor for --runs and no ceiling, so neither does this.
func TestALargeRunsValueIsNotRefusedForBeingLarge(t *testing.T) {
	if err := ValidateStepDispatch(clockStep("--runs", "100000")); err != nil {
		t.Fatalf("a large --runs was refused, but the tool states no ceiling: %v", err)
	}
}

func TestANegativeHoursIsRefusedAndZeroIsNot(t *testing.T) {
	if err := ValidateStepDispatch(clockStep("--hours", "-1")); err == nil {
		t.Fatal("--hours -1 was admitted; the tool answers it with exit 2")
	}
	if err := ValidateStepDispatch(clockStep("--hours", "0")); err != nil {
		t.Fatalf("--hours 0 was refused, but the tool only refuses a negative: %v", err)
	}
}

func TestANonIntegerValueIsRefusedBeforeTheTool(t *testing.T) {
	for _, args := range [][]string{{"--runs", "many"}, {"--hours", "1.5"}, {"--runs="}} {
		if err := ValidateStepDispatch(clockStep(args...)); err == nil {
			t.Fatalf("%v was admitted; flag.Parse cannot read it and the tool prints usage", args)
		}
	}
}

func TestARepoValueTheToolRefusesIsRefusedHere(t *testing.T) {
	for _, value := range []string{"not a repo", "bsikar", "bsikar/ra8/extra", "../ra8-firmware", "bsikar/..", "bsikar/ra8 firmware", ""} {
		if err := ValidateStepDispatch(clockStep("--repo", value)); err == nil {
			t.Fatalf("--repo %q was admitted; runner-clock refuses it", value)
		}
	}
}

func TestTheDefaultRepoShapeIsAdmitted(t *testing.T) {
	for _, value := range []string{"bsikar/ra8-firmware", "octo_cat/repo.name", "a/b"} {
		if err := ValidateStepDispatch(clockStep("--repo", value)); err != nil {
			t.Fatalf("--repo %q was refused: %v", value, err)
		}
	}
}

// Every judged flag is one the option door already says the tool parses, and
// every value-taking flag has a value rule. If either drifts the tables
// disagree and this says so.
func TestEveryJudgedFlagIsOneItsToolParsesAndTakesAValue(t *testing.T) {
	for program, rules := range toolFlagValueRules {
		accepted, stated := ReviewedToolFlags(program)
		if !stated {
			t.Fatalf("%s states value rules but no reviewed flags", program)
		}
		valued := valueTakingToolFlags[program]
		for name := range rules {
			if !statesFlag(accepted, name) {
				t.Fatalf("%s states a value rule for %q, which it does not parse", program, name)
			}
			if !valued[name] {
				t.Fatalf("%s states a value rule for %q, which is not recorded as taking a value", program, name)
			}
		}
		for name := range valued {
			if _, judged := rules[name]; !judged {
				t.Fatalf("%s parses %q with a value and nothing judges it", program, name)
			}
		}
	}
}

// A flag whose value is missing entirely is the tool's usage line too.
func TestAValueTakingFlagWithNoValueIsRefused(t *testing.T) {
	err := ValidateStepDispatch(clockStep("--runs"))
	if err == nil {
		t.Fatal("--runs with no value was admitted")
	}
	if !strings.Contains(err.Error(), "no value") {
		t.Fatalf("refusal does not say the value is missing: %v", err)
	}
}

// A mode switch on another tool carries no value and must stay untouched.
func TestAToolWithNoValueTakingFlagsIsUnjudged(t *testing.T) {
	if err := ValidateStepDispatch(Step{Name: "s", Program: "ra8ci:legacy-make", Args: []string{"--selftest"}}); err != nil {
		t.Fatalf("a valueless mode switch was refused: %v", err)
	}
}

// The embedded catalog is the manifest the fleet dispatches, so it has to pass
// the new door as it stands.
func TestTheEmbeddedCatalogIsAdmittedByTheValueDoor(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("embedded catalog refused: %v", err)
	}
	for _, name := range loaded.Names() {
		task, found := loaded.Task(name)
		if !found {
			t.Fatalf("catalog names %q but does not carry it", name)
		}
		for _, step := range task.Steps {
			if _, named := ToolProgram(step.Program); !named {
				continue
			}
			if err := checkToolFlagValuesAreOnesTheToolAccepts(step, step.Program); err != nil {
				t.Fatalf("reviewed task %q step %q refused: %v", name, step.Name, err)
			}
		}
	}
}
