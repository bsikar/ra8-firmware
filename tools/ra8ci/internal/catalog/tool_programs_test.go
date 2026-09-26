// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
)

func toolStep(program string) Step {
	return Step{Name: "gate", Program: program}
}

func TestAnUnimplementedToolProgramIsRefused(t *testing.T) {
	err := ValidateStepDispatch(toolStep("ra8ci:not-a-tool"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
	if !strings.Contains(err.Error(), "not-a-tool") {
		t.Fatalf("refusal does not name the tool: %v", err)
	}
}

func TestEveryReviewedToolProgramIsAdmitted(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if err := ValidateStepDispatch(toolStep(program)); err != nil {
			t.Fatalf("reviewed tool %q refused: %v", program, err)
		}
	}
}

func TestTheShippedCatalogNamesOnlyReviewedTools(t *testing.T) {
	raw := embedded.Manifest()
	catalog, err := Parse(raw, digestOf(t, raw))
	if err != nil {
		t.Fatalf("parse embedded catalog: %v", err)
	}
	named := 0
	for _, name := range catalog.Names() {
		task, found := catalog.Task(name)
		if !found {
			t.Fatalf("catalog named %q and does not hold it", name)
		}
		for _, step := range task.Steps {
			if _, isTool := ToolProgram(step.Program); !isTool {
				continue
			}
			named++
			if !IsReviewedToolProgram(step.Program) {
				t.Fatalf("task %q step %q names unimplemented tool %q", name, step.Name, step.Program)
			}
		}
	}
	if named == 0 {
		t.Fatal("the embedded catalog names no ra8ci tool at all, so this test proves nothing")
	}
}

func TestAnInvalidToolNameIsRefusedBeforeTheRegistry(t *testing.T) {
	// A name the registry could never hold is refused for its shape, so the
	// registry refusal never has to describe an unreadable name.
	err := ValidateStepDispatch(toolStep("ra8ci:Not A Tool"))
	if !errors.Is(err, ErrInvalidCatalog) || !strings.Contains(err.Error(), "invalid ra8ci tool") {
		t.Fatalf("want the shape refusal, got %v", err)
	}
}

func TestTheRegistryIsSortedAndFreeOfRepeats(t *testing.T) {
	programs := ReviewedToolPrograms()
	seen := make(map[string]bool, len(programs))
	for i, program := range programs {
		if !strings.HasPrefix(program, ToolProgramPrefix) {
			t.Fatalf("%q is not an %s program", program, ToolProgramPrefix)
		}
		if seen[program] {
			t.Fatalf("%q is listed twice", program)
		}
		seen[program] = true
		if i > 0 && programs[i-1] >= program {
			t.Fatalf("registry is not sorted at %q", program)
		}
	}
}

func TestReviewedToolProgramsCannotBeEditedThroughTheCopy(t *testing.T) {
	programs := ReviewedToolPrograms()
	programs[0] = "ra8ci:tampered"
	if IsReviewedToolProgram("ra8ci:tampered") {
		t.Fatal("a caller edited the registry through the returned slice")
	}
}

func TestAReviewedScriptStepIsUntouchedByTheRegistry(t *testing.T) {
	step := Step{Name: "gate", Program: DispatchShell, Args: []string{"tools/ci/gate.sh"}}
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("bash dispatch refused: %v", err)
	}
}
