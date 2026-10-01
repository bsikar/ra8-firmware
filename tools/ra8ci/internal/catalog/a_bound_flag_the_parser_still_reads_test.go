// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func behindTask(schema ArgsSchema, args ...string) Task {
	return Task{
		Name:       "rewrite",
		ArgsSchema: schema,
		Steps:      []Step{{Name: "ascii", Program: ToolProgramPrefix + "ascii", Args: args}},
	}
}

// The reviewed step names the target, so the bound flag can only land behind
// it.
func TestABoundFlagBehindAReviewedTargetIsRefused(t *testing.T) {
	task := behindTask(ArgsSchema{Flags: []string{"checkout"}}, "src/a.c")
	if err := checkNoBoundFlagLandsBehindATarget(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--checkout=value arrives after src/a.c, so ascii reads it as a target: %v", err)
	}
}

// The task supplies the target itself, and binding writes flags after
// positionals whatever the step says.
func TestABoundFlagBehindADeclaredPositionalIsRefused(t *testing.T) {
	task := behindTask(ArgsSchema{Positional: []string{"target"}, Flags: []string{"checkout"}})
	if err := checkNoBoundFlagLandsBehindATarget(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("binding writes the positional first and the flag behind it: %v", err)
	}
}

// No target anywhere on the line means the parser never stops, and the bound
// flag is read as the option it claims to be.
func TestABoundFlagWithNoTargetOnTheLineIsAdmitted(t *testing.T) {
	task := behindTask(ArgsSchema{Flags: []string{"checkout"}}, "--check")
	if err := checkNoBoundFlagLandsBehindATarget(task); err != nil {
		t.Fatalf("nothing ends option parsing here: %v", err)
	}
}

// Declaring no flag at all is the count question, and the target-ceiling door
// answers it.
func TestADeclaredPositionalAloneIsNotJudgedHere(t *testing.T) {
	task := behindTask(ArgsSchema{Positional: []string{"target"}})
	if err := checkNoBoundFlagLandsBehindATarget(task); err != nil {
		t.Fatalf("the ceiling door counts positionals; this one reads order: %v", err)
	}
}

// A tool that compares argv element by element finds its flag wherever
// binding puts it.
func TestAToolThatScansEveryElementTakesABoundFlagAnywhere(t *testing.T) {
	task := Task{
		Name:       "scan",
		ArgsSchema: ArgsSchema{Positional: []string{"target"}, Flags: []string{"selftest"}},
		Steps:      []Step{{Name: "casts", Program: ToolProgramPrefix + "assert-casts", Args: []string{"src/a.c"}}},
	}
	if err := checkNoBoundFlagLandsBehindATarget(task); err != nil {
		t.Fatalf("assert-casts reads its whole argv: %v", err)
	}
}

// A value-taking flag's value is not a target, so it does not end option
// parsing and does not make a bound flag late.
func TestAReviewedFlagValueIsNotATargetHere(t *testing.T) {
	task := Task{
		Name:       "clock",
		ArgsSchema: ArgsSchema{Flags: []string{"hours"}},
		Steps:      []Step{{Name: "clock", Program: ToolProgramPrefix + "runner-clock", Args: []string{"--repo", "bsikar/ra8-firmware"}}},
	}
	if err := checkNoBoundFlagLandsBehindATarget(task); err != nil {
		t.Fatalf("bsikar/ra8-firmware is --repo's value: %v", err)
	}
}

func TestTheRefusalNamesTheFlagAndTheStep(t *testing.T) {
	err := checkNoBoundFlagLandsBehindATarget(behindTask(ArgsSchema{Flags: []string{"checkout"}}, "src/a.c"))
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{"checkout", "ascii"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q: %v", want, err)
		}
	}
}

// Every tool this door judges is one the reviewed order door judges, so the
// two halves of the rule cannot drift apart.
func TestBothHalvesReadTheSameParserTable(t *testing.T) {
	for program := range toolsStoppingAtTheFirstTarget {
		if !ToolStopsAtTheFirstTarget(program) {
			t.Fatalf("%s is judged here but not by the reviewed door", program)
		}
	}
}

// Wired into the task validator, which is where a manifest is admitted.
func TestTheTaskValidatorRefusesABoundFlagBehindATarget(t *testing.T) {
	err := ValidateTaskDispatch(behindTask(ArgsSchema{Flags: []string{"checkout"}}, "src/a.c"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want a refusal: %v", err)
	}
	if !strings.Contains(err.Error(), "arrive as another target") {
		t.Fatalf("want this door's refusal: %v", err)
	}
}

func TestTheShippedCatalogBindsNoFlagBehindATarget(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
