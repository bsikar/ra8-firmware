// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// boundTask returns a task that declares the given schema and dispatches one
// step at the named tool. The fixture is asserted admitted BEFORE the rule is
// exercised, so a later failure is this door and not the seam around it.
func boundTask(t *testing.T, schema ArgsSchema, program string, args ...string) Task {
	t.Helper()
	task := Task{
		Name: "bound-argument-fixture", Version: 1, Tier: "required",
		Scope: "safe-local-read-only", OS: []string{"linux"},
		DeadlineSeconds: 600, BoardPolicy: "none",
		Retry:      RetryPolicy{MaxAttempts: 1},
		ArgsSchema: schema,
		Steps:      []Step{{Name: "gate", Program: program, Args: args}},
	}
	if err := ValidateStepDispatch(task.Steps[0]); err != nil {
		t.Fatalf("fixture step is not admitted before the rule is exercised: %v", err)
	}
	// The bare task is only a fair pre-check where the schema is not what
	// answers the scope question: for a scope-taking tool a declared
	// positional IS the scope, so stripping it refuses the fixture for a
	// reason that has nothing to do with this door.
	if !ToolRequiresAScopeSelector(program) {
		bare := task
		bare.ArgsSchema = ArgsSchema{}
		if err := ValidateReviewedTask(bare); err != nil {
			t.Fatalf("fixture is not admitted before the rule is exercised: %v", err)
		}
	}
	return task
}

func TestADeclaredPositionalIsAdmittedForAToolThatReadsFiles(t *testing.T) {
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}}, "ra8ci:ascii", "--checkout")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("ascii reads file arguments, so a bound path is its real dispatch: %v", err)
	}
}

func TestADeclaredPositionalIsRefusedForAToolThatReadsNoFiles(t *testing.T) {
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}}, "ra8ci:legacy-make")
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
	if !strings.Contains(err.Error(), "path") || !strings.Contains(err.Error(), "ra8ci:legacy-make") {
		t.Fatalf("refusal should name the argument and the tool: %v", err)
	}
}

func TestADeclaredPositionalIsRefusedForTheToolThatIgnoresIt(t *testing.T) {
	// wave-references does not exit 2: it ignores argv past --selftest and
	// files a verdict for a scope nobody declared, which is the worse half.
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}}, "ra8ci:wave-references")
	if !errors.Is(ValidateReviewedTask(task), ErrInvalidCatalog) {
		t.Fatal("a bound path silently ignored by the tool must not be admitted")
	}
}

func TestADeclaredFlagIsAdmittedWhenTheToolParsesIt(t *testing.T) {
	task := boundTask(t, ArgsSchema{Flags: []string{"hours"}}, "ra8ci:runner-clock")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("runner-clock parses --hours, so binding it is how the option is supplied: %v", err)
	}
}

func TestADeclaredFlagIsRefusedWhenTheToolParsesNoSuchOption(t *testing.T) {
	task := boundTask(t, ArgsSchema{Flags: []string{"since"}}, "ra8ci:runner-clock")
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
	if !strings.Contains(err.Error(), "--since=value") {
		t.Fatalf("refusal should show the spelling binding produces: %v", err)
	}
}

func TestADeclaredFlagIsRefusedForAToolWithOnlyASelfTest(t *testing.T) {
	task := boundTask(t, ArgsSchema{Flags: []string{"all"}}, "ra8ci:legacy-make")
	if !errors.Is(ValidateReviewedTask(task), ErrInvalidCatalog) {
		t.Fatal("legacy-make parses only --selftest")
	}
}

func TestEveryDeclaredFlagIsJudgedNotOnlyTheFirst(t *testing.T) {
	task := boundTask(t, ArgsSchema{Flags: []string{"all", "nonsense"}}, "ra8ci:no-null", "--selftest")
	if !errors.Is(ValidateReviewedTask(task), ErrInvalidCatalog) {
		t.Fatal("a later declared flag the tool cannot parse must be refused too")
	}
}

func TestADeclaredSchemaIsNotJudgedAgainstAScriptStep(t *testing.T) {
	// A reviewed script reads whatever it likes off its own argv, so this
	// door has nothing to say about a bash dispatch.
	task := boundTask(t, ArgsSchema{Positional: []string{"path"}, Flags: []string{"anything"}},
		"bash", "scripts/checks/format_tree.sh")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a script step is not judged by this door: %v", err)
	}
}

func TestATaskDeclaringNoArgumentsIsUntouched(t *testing.T) {
	task := boundTask(t, ArgsSchema{}, "ra8ci:legacy-make")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a task that declares nothing binds nothing: %v", err)
	}
}

func TestTheDoorJudgesTheDeclaredFlagNotItsValue(t *testing.T) {
	// The value arrives at dispatch. ascii parses --check, so the flag is
	// admitted here even though --check=yes would fail at the tool and
	// --check=true would not: review cannot pin which a caller sends.
	task := boundTask(t, ArgsSchema{Flags: []string{"check"}}, "ra8ci:ascii", "--all")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a parsed flag is admitted whatever value a caller may supply: %v", err)
	}
}

func TestTheShippedCatalogIsAdmittedByThisDoor(t *testing.T) {
	catalog, err := Load()
	if err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
	if len(catalog.Names()) == 0 {
		t.Fatal("embedded catalog is empty")
	}
}

func TestEveryToolTheDoorReadsStatesBothTables(t *testing.T) {
	for _, program := range ReviewedToolPrograms() {
		if _, known := reviewedToolFlags[program]; !known {
			t.Errorf("%s states no parsed flags, so a declared flag could not be judged", program)
		}
	}
	for program := range toolsReadingFileArguments {
		if _, known := reviewedToolFlags[program]; !known {
			t.Errorf("%s reads file arguments but states no parsed flags", program)
		}
	}
}
