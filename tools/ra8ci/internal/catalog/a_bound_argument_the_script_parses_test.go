// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

const boundScript = "scripts/ci.sh"

// scriptBoundTask builds a task whose single step dispatches a stated script,
// so only the schema under test varies. The step itself is admitted first, so
// a refusal here is this door's and not an earlier one's.
func scriptBoundTask(t *testing.T, schema ArgsSchema, args ...string) Task {
	t.Helper()
	task := Task{
		Name: "bound-script-fixture", Version: 1, Tier: "required",
		Scope: "safe-local-read-only", OS: []string{"linux"},
		DeadlineSeconds: 600, BoardPolicy: "none",
		Retry:      RetryPolicy{MaxAttempts: 1},
		ArgsSchema: schema,
		Steps:      []Step{{Name: "gate", Program: DispatchShell, Args: args}},
	}
	if err := ValidateStepDispatch(task.Steps[0]); err != nil {
		t.Fatalf("fixture step is not admitted before the rule is exercised: %v", err)
	}
	bare := task
	bare.ArgsSchema = ArgsSchema{}
	if err := ValidateReviewedTask(bare); err != nil {
		t.Fatalf("fixture is not admitted before the rule is exercised: %v", err)
	}
	return task
}

func TestADeclaredPositionalIsRefusedAgainstAScriptThatParsesOnlyOptions(t *testing.T) {
	task := scriptBoundTask(t, ArgsSchema{Positional: []string{"target"}}, boundScript, "--gate=format")
	err := ValidateTaskDispatch(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a bound positional reaching ci.sh must be refused, got %v", err)
	}
	if !strings.Contains(err.Error(), "target") || !strings.Contains(err.Error(), boundScript) {
		t.Fatalf("the refusal must name the argument and the script, got %q", err)
	}
}

func TestADeclaredFlagTheScriptParsesWithAnEqualsArmIsAdmitted(t *testing.T) {
	task := scriptBoundTask(t, ArgsSchema{Flags: []string{"gate"}}, boundScript, "--fast")
	if err := ValidateTaskDispatch(task); err != nil {
		t.Fatalf("--gate=value is exactly how ci.sh takes a gate name: %v", err)
	}
}

func TestADeclaredFlagTheScriptParsesOnlyBareIsRefused(t *testing.T) {
	for _, name := range []string{"fast", "native", "container", "rebuild"} {
		task := scriptBoundTask(t, ArgsSchema{Flags: []string{name}}, boundScript, "--gate=format")
		err := ValidateTaskDispatch(task)
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("binding spells %q as --%s=value, which ci.sh does not parse, got %v", name, name, err)
		}
		if !strings.Contains(err.Error(), "--"+name+"=value") {
			t.Fatalf("the refusal must name the spelling binding produces, got %q", err)
		}
	}
}

func TestTheRefusalSeparatesAKnownNameFromAnUnknownOne(t *testing.T) {
	known := ValidateTaskDispatch(scriptBoundTask(t, ArgsSchema{Flags: []string{"fast"}}, boundScript, "--gate=format"))
	if known == nil || !strings.Contains(known.Error(), "no --fast=value arm") {
		t.Fatalf("a name the script knows in another form must say so, got %v", known)
	}
	unknown := ValidateTaskDispatch(scriptBoundTask(t, ArgsSchema{Flags: []string{"gates"}}, boundScript, "--gate=format"))
	if unknown == nil || strings.Contains(unknown.Error(), "arm") {
		t.Fatalf("a name the script never heard of must not be reported as a spelling problem, got %v", unknown)
	}
}

func TestADeclaredFlagNamingAValueTakingSpellingWithoutAnEqualsArmIsRefused(t *testing.T) {
	task := scriptBoundTask(t, ArgsSchema{Flags: []string{"selftest-abort"}}, boundScript, "--fast")
	err := ValidateTaskDispatch(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("ci.sh shifts for --selftest-abort and has no =arm, so the bound spelling is unknown to it, got %v", err)
	}
}

func TestADeclaredFlagNamingAReportingModeIsRefused(t *testing.T) {
	task := scriptBoundTask(t, ArgsSchema{Flags: []string{"list-gates"}}, boundScript, "--fast")
	if !errors.Is(ValidateTaskDispatch(task), ErrInvalidCatalog) {
		t.Fatal("--list-gates=value reaches the unknown-flag arm like any other =form")
	}
}

func TestASchemaDeclaringNothingIsAdmitted(t *testing.T) {
	task := scriptBoundTask(t, ArgsSchema{}, boundScript, "--gate=format")
	if err := ValidateTaskDispatch(task); err != nil {
		t.Fatalf("a task binding nothing adds no argv: %v", err)
	}
}

func TestAScriptWithNoStatedContractIsAdmittedOnItsPathAlone(t *testing.T) {
	const unstated = "scripts/checks/format_tree.sh"
	if ReviewedScriptStatesItsOptions(unstated) {
		t.Fatalf("%s now states a contract; pick another unstated script for this test", unstated)
	}
	task := scriptBoundTask(t, ArgsSchema{Positional: []string{"target"}, Flags: []string{"fast"}}, unstated)
	if err := ValidateTaskDispatch(task); err != nil {
		t.Fatalf("this door extends where a contract is stated, not everywhere: %v", err)
	}
}

func TestAToolStepIsLeftToTheToolSideDoors(t *testing.T) {
	task := scriptBoundTask(t, ArgsSchema{}, boundScript, "--fast")
	task.Steps = []Step{{Name: "gate", Program: "ra8ci:assert-casts", Args: []string{"src/ra8_batt.c"}}}
	task.ArgsSchema = ArgsSchema{Flags: []string{"fast"}}
	err := ValidateTaskDispatch(task)
	if err == nil {
		t.Fatal("the tool-side door refuses this; the fixture is meant to prove which one speaks")
	}
	if strings.Contains(err.Error(), boundScript) {
		t.Fatalf("a tool step must not be judged against a script contract, got %q", err)
	}
}

func TestScriptTakesABoundFlagAgreesWithTheDoor(t *testing.T) {
	if !ScriptTakesABoundFlag(boundScript, "gate") {
		t.Fatal("--gate=value is the one bound spelling ci.sh parses")
	}
	for _, name := range []string{"fast", "selftest-abort", "list-gates", "help", "nonsense"} {
		if ScriptTakesABoundFlag(boundScript, name) {
			t.Fatalf("ci.sh has no --%s=value arm", name)
		}
	}
	if ScriptTakesABoundFlag("scripts/checks/format_tree.sh", "gate") {
		t.Fatal("an unstated script takes no bound flag as far as review can tell")
	}
}

func TestEveryEqualsArmIsOneTheScriptAlsoParsesBare(t *testing.T) {
	for script, stated := range reviewedScriptOptions {
		for _, spelling := range stated.takingAnEqualsValue {
			if !statesExactly(stated.valueless, spelling) && !statesExactly(stated.takingTheNextArgument, spelling) {
				t.Fatalf("%s: %s has an =arm but no bare arm; check the script's parser before trusting this table", script, spelling)
			}
		}
	}
}

func TestTheShippedCatalogBindsNothingAScriptCannotParse(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
