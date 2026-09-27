// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func allStep(program string, args ...string) Step {
	return Step{Name: "gate", Program: program, Args: args}
}

func TestAWholeTreeScanRefusesAPathBesideIt(t *testing.T) {
	err := ValidateStepDispatch(allStep("ra8ci:no-null", "--all", "src/ra8_batt.c"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--all beside a path was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "whole tree") {
		t.Fatalf("refusal does not say why: %v", err)
	}
}

func TestEveryToolParsingAllRefusesAPathBesideIt(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		accepted, known := ReviewedToolFlags(program)
		if !known || !statesFlag(accepted, allFlag) {
			continue
		}
		if err := ValidateStepDispatch(allStep(program, "--all", "src/ra8_ui.c")); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s admitted --all beside a path: %v", program, err)
		}
	}
}

func TestAWholeTreeScanOnItsOwnIsAdmitted(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		accepted, known := ReviewedToolFlags(program)
		if !known || !statesFlag(accepted, allFlag) {
			continue
		}
		if err := ValidateStepDispatch(allStep(program, "--all")); err != nil {
			t.Fatalf("%s refused a plain whole-tree scan: %v", program, err)
		}
	}
}

func TestTheOneReviewedCompanionOfAllIsAdmitted(t *testing.T) {
	if err := ValidateStepDispatch(allStep("ra8ci:ascii", "--all", "--check")); err != nil {
		t.Fatalf("ascii --all --check refused: %v", err)
	}
	if err := ValidateStepDispatch(allStep("ra8ci:ascii", "--check", "--all")); err != nil {
		t.Fatalf("order changed the answer: %v", err)
	}
}

func TestAScopeContradictionIsRefused(t *testing.T) {
	err := ValidateStepDispatch(allStep("ra8ci:ascii", "--all", "--checkout"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--all --checkout was admitted: %v", err)
	}
}

func TestACompanionOfAnotherToolIsNotACompanionHere(t *testing.T) {
	if err := ValidateStepDispatch(allStep("ra8ci:assert-casts", "--all", "--check")); err == nil {
		t.Fatal("assert-casts admitted --check, which it does not parse")
	}
}

func TestTheAllDoorSaysNothingAboutAToolWithoutIt(t *testing.T) {
	if err := ValidateStepDispatch(allStep("ra8ci:legacy-make")); err != nil {
		t.Fatalf("a plain tool step was refused: %v", err)
	}
	if err := ValidateStepDispatch(allStep("ra8ci:gnu-attribute", "src/ra8_batt.c")); err != nil {
		t.Fatalf("a file-taking tool without --all was refused: %v", err)
	}
}

func TestTheAllDoorLeavesTheSelfTestDoorAlone(t *testing.T) {
	err := ValidateStepDispatch(allStep("ra8ci:no-null", "--selftest", "--all"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("--selftest --all was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "self test") {
		t.Fatalf("the all door answered for the self test: %v", err)
	}
}

func TestEveryAllCompanionIsOneItsToolParses(t *testing.T) {
	for program, companions := range allModeCompanions {
		accepted, known := ReviewedToolFlags(program)
		if !known {
			t.Fatalf("%s states --all companions but no reviewed flags", program)
		}
		if !statesFlag(accepted, allFlag) {
			t.Fatalf("%s states --all companions but does not parse --all", program)
		}
		for name := range companions {
			if !statesFlag(accepted, name) {
				t.Fatalf("%s states the companion %q, which it does not parse", program, name)
			}
		}
	}
}

func TestTheEmbeddedCatalogIsAdmittedByTheAllDoor(t *testing.T) {
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
			if err := checkAnAllStepNamesTheWholeTree(step, step.Program); err != nil {
				t.Fatalf("reviewed task %q step %q refused: %v", name, step.Name, err)
			}
		}
	}
}
