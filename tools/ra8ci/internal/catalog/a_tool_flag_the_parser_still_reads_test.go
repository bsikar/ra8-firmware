// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

const stoppingAsciiProgram = ToolProgramPrefix + "ascii"

func stoppingStep(args ...string) Step {
	return Step{Name: "ascii", Program: stoppingAsciiProgram, Args: args}
}

// The live entry, and the whole reason for the door: every element is right on
// its own, and what ascii receives is two targets and no --checkout.
func TestAnOptionWrittenAfterATargetIsRefused(t *testing.T) {
	for _, args := range [][]string{
		{"src/a.c", "--checkout"},
		{"src/a.c", "-checkout"},
		{"src/a.c", "--check"},
		{"--check", "src/a.c", "--checkout"},
		{"src/a.c", "--all"},
	} {
		if err := checkNoToolFlagFollowsATarget(stoppingStep(args...), stoppingAsciiProgram); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%v: the parser stopped at the target, so the option is another target: %v", args, err)
		}
	}
}

func TestTheRefusalNamesTheTargetThatEndedOptionParsing(t *testing.T) {
	err := checkNoToolFlagFollowsATarget(stoppingStep("src/a.c", "--checkout"), stoppingAsciiProgram)
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{"--checkout", "src/a.c"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q so a reader can see which element ended option parsing: %v", want, err)
		}
	}
}

// The same two elements the other way round are what the tool documents, and
// the shipped catalog writes them this way.
func TestAnOptionWrittenBeforeTheTargetIsAdmitted(t *testing.T) {
	for _, args := range [][]string{
		{"--checkout", "src/a.c"},
		{"--check", "--checkout", "src/a.c"},
		{"src/a.c"},
		{"--all"},
		{"--selftest"},
		nil,
	} {
		if err := checkNoToolFlagFollowsATarget(stoppingStep(args...), stoppingAsciiProgram); err != nil {
			t.Fatalf("%v is argv the parser reads whole: %v", args, err)
		}
	}
}

// A value-taking flag swallows the element after it, so that element is not a
// target and does not end option parsing. runner-clock is the only dispatched
// tool with such a flag.
func TestAFlagsValueDoesNotEndOptionParsing(t *testing.T) {
	clock := ToolProgramPrefix + "runner-clock"
	step := Step{Name: "clock", Program: clock, Args: []string{"--repo", "bsikar/ra8-firmware", "--runs", "5", "--hours", "24"}}
	if err := checkNoToolFlagFollowsATarget(step, clock); err != nil {
		t.Fatalf("bsikar/ra8-firmware is --repo's value, not a target: %v", err)
	}
}

// The equals spelling carries its own value, so the element after it is a
// target like any other.
func TestAnEqualsSpellingDoesNotSwallowTheNextElement(t *testing.T) {
	clock := ToolProgramPrefix + "runner-clock"
	step := Step{Name: "clock", Program: clock, Args: []string{"--repo=bsikar/ra8-firmware", "src/a.c", "--runs"}}
	if err := checkNoToolFlagFollowsATarget(step, clock); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--runs sits after a target the parser already stopped on: %v", err)
	}
}

// The fifteen tools that compare argv element by element find a flag wherever
// it sits, so refusing them here would refuse work they do correctly.
func TestAToolThatScansEveryElementIsNotJudgedHere(t *testing.T) {
	for _, tool := range []string{"ra8ci:assert-casts", "ra8ci:final-newline", "ra8ci:no-null", "ra8ci:since", "ra8ci:gnu-attribute", "ra8ci:tz-boundary-discard"} {
		step := Step{Name: "scan", Program: tool, Args: []string{"src/a.c", "--all"}}
		if err := checkNoToolFlagFollowsATarget(step, tool); err != nil {
			t.Fatalf("%s reads its whole argv: %v", tool, err)
		}
	}
}

// A tool the dispatch list does not state is admitted here on that alone; the
// program door settles it first.
func TestAnUnstatedProgramIsNotJudgedHere(t *testing.T) {
	step := Step{Name: "scan", Program: ToolProgramPrefix + "not-a-tool", Args: []string{"src/a.c", "--all"}}
	if err := checkNoToolFlagFollowsATarget(step, step.Program); err != nil {
		t.Fatalf("the program door names this one: %v", err)
	}
}

// Every tool judged here must be one the dispatch list states and one that
// states the flags it parses, so a tool cannot be renamed in one place and
// left behind in this table.
func TestEveryToolJudgedHereIsOneTheSeamStates(t *testing.T) {
	for program := range toolsStoppingAtTheFirstTarget {
		if _, named := ToolProgram(program); !named {
			t.Fatalf("%s is judged for argv order but is not a dispatched tool", program)
		}
		if _, states := ReviewedToolFlags(program); !states {
			t.Fatalf("%s is judged for argv order but states no flags to order", program)
		}
	}
}

// runner-clock and tests-readme read no file argument at all, so a target
// handed to either is the file door's refusal and reaches this one never.
// Asserted through the whole step validator, which is where the order holds.
func TestATargetHandedAToolThatReadsNoneStaysTheFileDoorsRefusal(t *testing.T) {
	step := Step{Name: "clock", Program: ToolProgramPrefix + "runner-clock", Args: []string{"src/a.c", "--selftest"}}
	err := ValidateStepDispatch(step)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want a refusal: %v", err)
	}
	if strings.Contains(err.Error(), "stops reading options") {
		t.Fatalf("the file door names the real mistake, not argv order: %v", err)
	}
}

// A dash naming no option is the option door's refusal wherever it sits, and
// this door must not take it over.
func TestADashNamingNoOptionStaysTheOptionDoorsRefusal(t *testing.T) {
	err := ValidateStepDispatch(stoppingStep("src/a.c", "--"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want a refusal: %v", err)
	}
	if strings.Contains(err.Error(), "stops reading options") {
		t.Fatalf("a bare dash is the option door's refusal: %v", err)
	}
}

// The door is wired into the tool branch, so a step written this way is
// refused where a manifest is admitted rather than only in the unit.
func TestTheStepValidatorRefusesAnOptionAfterATarget(t *testing.T) {
	err := ValidateStepDispatch(stoppingStep("src/a.c", "--checkout"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want a refusal: %v", err)
	}
	if !strings.Contains(err.Error(), "stops reading options") {
		t.Fatalf("want this door's refusal: %v", err)
	}
}

func TestTheShippedCatalogWritesNoOptionAfterATarget(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
