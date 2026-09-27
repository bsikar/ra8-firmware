// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

var scopeRequiringTools = []string{"ra8ci:ascii", "ra8ci:assert-casts", "ra8ci:no-null", "ra8ci:since"}

// scopedTask builds the smallest reviewed task carrying one tool step, so
// every assertion below runs through ValidateTaskDispatch rather than calling
// the door directly.
func scopedTask(program string, args ...string) Task {
	return Task{
		Name:  "scope-task",
		Steps: []Step{{Name: "scope-step", Program: program, Args: args}},
		Retry: RetryPolicy{MaxAttempts: 1},
	}
}

func TestAToolThatCannotDeriveScopeIsRefusedAnEmptyArgv(t *testing.T) {
	for _, program := range scopeRequiringTools {
		err := ValidateTaskDispatch(scopedTask(program))
		if err == nil {
			t.Fatalf("%s with no arguments was admitted; it answers an empty argv with a usage error on every runner", program)
		}
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s: refusal is not an invalid-catalog error: %v", program, err)
		}
		if !strings.Contains(err.Error(), "no scope") {
			t.Fatalf("%s: refusal does not say what is missing: %v", program, err)
		}
	}
}

func TestAPathIsAScopeEveryRequiringToolAccepts(t *testing.T) {
	for _, program := range scopeRequiringTools {
		if err := ValidateTaskDispatch(scopedTask(program, "src/ra8_batt.c")); err != nil {
			t.Fatalf("%s with one path was refused: %v", program, err)
		}
	}
}

func TestTheWholeTreeIsAScopeEveryRequiringToolAccepts(t *testing.T) {
	for _, program := range scopeRequiringTools {
		if err := ValidateTaskDispatch(scopedTask(program, "--all")); err != nil {
			t.Fatalf("%s --all was refused: %v", program, err)
		}
	}
}

func TestTheSelfTestIsAScopeEveryRequiringToolAccepts(t *testing.T) {
	for _, program := range scopeRequiringTools {
		if err := ValidateTaskDispatch(scopedTask(program, "--selftest")); err != nil {
			t.Fatalf("%s --selftest was refused: %v", program, err)
		}
	}
}

// The single-dash and --name=value spellings read the same here as in every
// other door on this seam, because all of them ask flagNameArgvClaims what an
// argument is called.
func TestAScopeIsReadInEitherDashSpelling(t *testing.T) {
	if err := ValidateTaskDispatch(scopedTask("ra8ci:no-null", "-all")); err != nil {
		t.Fatalf("-all was refused as a scope: %v", err)
	}
	if err := ValidateTaskDispatch(scopedTask("ra8ci:assert-casts", "-selftest")); err != nil {
		t.Fatalf("-selftest was refused as a scope: %v", err)
	}
}

// The shape the embedded catalog actually ships: the reviewed argv names no
// target and the task's declared positional supplies it at dispatch.
func TestADeclaredPositionalIsTheScopeTheCallerSupplies(t *testing.T) {
	task := scopedTask("ra8ci:ascii", "--checkout")
	task.ArgsSchema.Positional = []string{"path"}
	if err := ValidateTaskDispatch(task); err != nil {
		t.Fatalf("a task declaring a positional was refused: %v", err)
	}
}

// A declared FLAG is not a scope: BindArguments appends it as --name=value,
// which is neither a path nor a switch any of these four reads as scope.
func TestADeclaredFlagIsNotAScope(t *testing.T) {
	task := scopedTask("ra8ci:no-null")
	task.ArgsSchema.Flags = []string{"mode"}
	if err := ValidateTaskDispatch(task); err == nil {
		t.Fatal("a task declaring only a flag was admitted; a bound --mode=x is not a target")
	}
}

// --check chooses report-instead-of-rewrite. It answers what to DO, and
// parseOptions still requires a target or --all beside it.
func TestAsciiCheckAloneIsNotAScope(t *testing.T) {
	err := ValidateTaskDispatch(scopedTask("ra8ci:ascii", "--check"))
	if err == nil {
		t.Fatal("ascii --check alone was admitted; parseOptions requires one target or --all beside it")
	}
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("refusal is not an invalid-catalog error: %v", err)
	}
}

// --checkout says where to resolve the target, not which target.
func TestAsciiCheckoutAloneIsNotAScope(t *testing.T) {
	if err := ValidateTaskDispatch(scopedTask("ra8ci:ascii", "--checkout")); err == nil {
		t.Fatal("ascii --checkout alone, with nothing declared, was admitted; the target is still required")
	}
	if err := ValidateTaskDispatch(scopedTask("ra8ci:ascii", "--checkout", "src/ra8_batt.c")); err != nil {
		t.Fatalf("ascii --checkout with its target was refused: %v", err)
	}
}

// The fourteen tools that derive their own scope must keep taking an empty
// argv: it is the shape the shipped manifest dispatches them with.
func TestAToolDerivingItsOwnScopeStillTakesAnEmptyArgv(t *testing.T) {
	for _, program := range toolsDerivingTheirOwnScope() {
		if err := ValidateTaskDispatch(scopedTask(program)); err != nil {
			t.Fatalf("%s with no arguments was refused, but it derives its own scope: %v", program, err)
		}
	}
}

// The set has to stay level with the dispatch list, so a tool cannot be added
// in one place and forgotten here.
func TestEveryReviewedToolSaysWhetherItRequiresAScopeSelector(t *testing.T) {
	deriving := len(toolsDerivingTheirOwnScope())
	requiring := len(toolsRequiringAScopeSelector)
	if deriving+requiring != len(ReviewedToolPrograms()) {
		t.Fatalf("%d tools derive their own scope and %d require a selector, but %d are dispatched",
			deriving, requiring, len(ReviewedToolPrograms()))
	}
	for program := range toolsRequiringAScopeSelector {
		if !IsReviewedToolProgram(program) {
			t.Fatalf("%q requires a scope selector but is not dispatched", program)
		}
	}
}

// Every tool named here also reads file arguments, which is what makes a path
// an available scope for all four. If that stops being true the two tables
// disagree and this says so.
func TestEveryScopeRequiringToolReadsFileArguments(t *testing.T) {
	for program := range toolsRequiringAScopeSelector {
		if !ToolReadsFileArguments(program) {
			t.Fatalf("%q requires a scope selector but reads no file arguments, so a path cannot be one", program)
		}
	}
}

func TestToolRequiresAScopeSelectorAnswersForTheWholeDispatchList(t *testing.T) {
	if !ToolRequiresAScopeSelector("ra8ci:since") {
		t.Fatal("since is reported as deriving its own scope; its empty argv falls to the usage branch")
	}
	if ToolRequiresAScopeSelector("ra8ci:gnu-attribute") {
		t.Fatal("gnu-attribute is reported as requiring a selector; an empty argv discovers the tree")
	}
	if ToolRequiresAScopeSelector("ra8ci:not-a-tool") {
		t.Fatal("an unlisted name is reported as requiring a selector")
	}
}

// The embedded catalog is the manifest the fleet actually dispatches, so it
// has to pass the new door as it stands.
func TestTheEmbeddedCatalogIsAdmittedByTheScopeDoor(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("embedded catalog refused: %v", err)
	}
	for _, name := range loaded.Names() {
		task, found := loaded.Task(name)
		if !found {
			t.Fatalf("catalog names %q but does not carry it", name)
		}
		if err := checkAScopeSelectorIsNamedWhereTheToolRequiresOne(task); err != nil {
			t.Fatalf("reviewed task %q refused: %v", name, err)
		}
	}
}
