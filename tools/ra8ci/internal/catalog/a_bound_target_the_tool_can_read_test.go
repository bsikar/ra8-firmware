// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// targetBoundTask builds a reviewed task whose one step dispatches program
// with args, under the given schema.
func targetBoundTask(t *testing.T, schema ArgsSchema, program string, args ...string) Task {
	t.Helper()
	task := Task{
		Name: "bound-target-fixture", Version: 1, Tier: "required",
		Scope: "safe-local-read-only", OS: []string{"linux"},
		DeadlineSeconds: 600, BoardPolicy: "none",
		Retry:      RetryPolicy{MaxAttempts: 1},
		ArgsSchema: schema,
		Steps:      []Step{{Name: "gate", Program: program, Args: args}},
	}
	if err := ValidateStepDispatch(task.Steps[0]); err != nil {
		t.Fatalf("fixture step is not admitted before the rule is exercised: %v", err)
	}
	return task
}

func TestABoundTargetBesideANamedOneIsRefused(t *testing.T) {
	task := targetBoundTask(t, ArgsSchema{Positional: []string{"target"}}, "ra8ci:ascii", "src/ra8_batt.c")
	err := checkBoundTargetsFitTheToolsCeiling(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a bound target beside a named one was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "exactly one") {
		t.Fatalf("refusal does not say the ceiling: %v", err)
	}
}

func TestTwoBoundTargetsAreRefusedAgainstAStepNamingNone(t *testing.T) {
	task := targetBoundTask(t, ArgsSchema{Positional: []string{"first", "second"}}, "ra8ci:ascii")
	if err := checkBoundTargetsFitTheToolsCeiling(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("two bound targets were admitted: %v", err)
	}
}

func TestOneBoundTargetIsHowTheToolIsMeantToBeSupplied(t *testing.T) {
	task := targetBoundTask(t, ArgsSchema{Positional: []string{"target"}}, "ra8ci:ascii")
	if err := checkBoundTargetsFitTheToolsCeiling(task); err != nil {
		t.Fatalf("the one target a caller supplies was refused: %v", err)
	}
	withOption := targetBoundTask(t, ArgsSchema{Positional: []string{"target"}}, "ra8ci:ascii", "--checkout")
	if err := checkBoundTargetsFitTheToolsCeiling(withOption); err != nil {
		t.Fatalf("an option beside the one bound target was refused: %v", err)
	}
}

func TestADeclaredFlagChangesNoTargetCount(t *testing.T) {
	task := targetBoundTask(t, ArgsSchema{Flags: []string{"check"}}, "ra8ci:ascii", "src/ra8_batt.c")
	if err := checkBoundTargetsFitTheToolsCeiling(task); err != nil {
		t.Fatalf("a declared flag was counted as a target: %v", err)
	}
}

func TestAToolScanningEveryTargetTakesBoundOnesToo(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if !ToolReadsFileArguments(program) {
			continue
		}
		if _, stated := ToolTargetCeiling(program); stated {
			continue
		}
		task := targetBoundTask(t, ArgsSchema{Positional: []string{"first", "second"}}, program, "src/ra8_batt.c")
		if err := checkBoundTargetsFitTheToolsCeiling(task); err != nil {
			t.Fatalf("%s refused bound targets it scans: %v", program, err)
		}
	}
}

func TestATaskDeclaringNothingIsUntouched(t *testing.T) {
	task := targetBoundTask(t, ArgsSchema{}, "ra8ci:ascii", "src/ra8_batt.c")
	if err := checkBoundTargetsFitTheToolsCeiling(task); err != nil {
		t.Fatalf("a task declaring no arguments was refused: %v", err)
	}
}

func TestTheBoundTargetDoorRunsThroughTaskDispatch(t *testing.T) {
	task := targetBoundTask(t, ArgsSchema{Positional: []string{"target"}}, "ra8ci:ascii", "src/ra8_batt.c")
	if err := ValidateTaskDispatch(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the door is not wired into ValidateTaskDispatch: %v", err)
	}
}

func TestTheShippedCatalogBindsNoExtraTarget(t *testing.T) {
	source, err := Load()
	if err != nil {
		t.Fatalf("the shipped catalog no longer loads: %v", err)
	}
	for _, name := range source.Names() {
		task, found := source.Task(name)
		if !found {
			t.Fatalf("the shipped catalog names %q and does not hold it", name)
		}
		if err := checkBoundTargetsFitTheToolsCeiling(task); err != nil {
			t.Fatalf("shipped task %q: %v", name, err)
		}
	}
}
