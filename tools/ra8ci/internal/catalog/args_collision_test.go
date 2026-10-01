// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// collisionTask is a reviewed definition the admission rules admit: one
// dispatched script, no declared arguments. Each test declares the schema and
// the reviewed arguments it is actually about.
func collisionTask() Task {
	return Task{
		Name: "rewrite", Version: 1, Tier: "optional", Scope: "safe-local-write-working-tree",
		OS: []string{"linux"}, DeadlineSeconds: 300, BoardPolicy: "none",
		Steps: []Step{{Name: "rewrite-path", Program: DispatchShell,
			Args: []string{"scripts/checks/format_tree" + ScriptPathSuffix}}},
		Retry: RetryPolicy{MaxAttempts: 1},
	}
}

func withReviewedArgs(task Task, args ...string) Task {
	task.Steps[0].Args = append(append([]string(nil), task.Steps[0].Args...), args...)
	return task
}

func TestAReviewedArgumentMayNotCarryADeclaredFlagName(t *testing.T) {
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "--mode=strict")
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	for _, want := range []string{"rewrite", `"mode"`, "rewrite-path", "--mode=strict"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal must name %q, got %v", want, err)
		}
	}
}

func TestABareReviewedFlagCollidesTheSameWay(t *testing.T) {
	// --mode with no value is the boolean form of the same flag, and a bound
	// --mode=fast lands after it just as surely.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "--mode")
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal for the bare flag form, got %v", err)
	}
}

func TestAReviewedFlagThatIsNotDeclaredIsStillAdmitted(t *testing.T) {
	// The ONE task in the v1 catalog that declares a schema is exactly this
	// shape: ascii-rewrite passes a reviewed --checkout and declares a
	// positional. Nothing about it collides, and it must keep running.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}}
	task = withReviewedArgs(task, "--checkout")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a reviewed flag the schema does not declare must be admitted: %v", err)
	}
}

func TestAPrefixOfADeclaredFlagDoesNotCollide(t *testing.T) {
	// --mode-file is a different flag from --mode, and refusing it would be
	// this rule reading names by prefix rather than by name.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "--mode-file=review.json")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a longer reviewed flag name must be admitted: %v", err)
	}
}

func TestAReviewedEndOfOptionsMarkerIsRefusedWhenArgumentsAreDeclared(t *testing.T) {
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "--")
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal, got %v", err)
	}
	if !strings.Contains(err.Error(), "operand") {
		t.Fatalf("refusal must say what the marker does to a bound argument, got %v", err)
	}
}

func TestAReviewedEndOfOptionsMarkerIsAdmittedWhenNoArgumentsAreDeclared(t *testing.T) {
	// 85 tasks declare no schema. This rule has nothing to say about any of
	// them, whatever their reviewed argv looks like.
	task := collisionTask()
	task = withReviewedArgs(task, "--", "--mode=strict")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a task declaring no arguments must be admitted: %v", err)
	}
}

func TestASingleDashReviewedArgumentDoesNotCollide(t *testing.T) {
	// "-" is stdin to most programs, not an end-of-options marker.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}}
	task = withReviewedArgs(task, "-")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a reviewed %q must be admitted: %v", "-", err)
	}
}

func TestTheCollisionRuleReadsEveryStep(t *testing.T) {
	// A task declaring arguments runs one step today (checkArgumentsReachOneStep),
	// so this pins the rule on the function rather than through admission.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task.Steps = append(task.Steps, Step{Name: "selftest", Program: DispatchShell,
		Args: []string{"scripts/checks/format_tree" + ScriptPathSuffix, "--mode=strict"}})
	if err := checkBoundArgumentsKeepTheirMeaning(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a refusal naming the later step, got %v", err)
	}
}

func TestEveryEmbeddedTaskKeepsItsBoundArgumentsMeaning(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("embedded catalog must load: %v", err)
	}
	for _, name := range loaded.Names() {
		task, ok := loaded.Task(name)
		if !ok {
			t.Fatalf("catalog names %q and does not hold it", name)
		}
		if err := checkBoundArgumentsKeepTheirMeaning(task); err != nil {
			t.Fatalf("embedded task %q must be admitted: %v", name, err)
		}
	}
}

func TestASingleDashReviewedFlagCollidesTheSameWay(t *testing.T) {
	// Go's flag package reads -mode and --mode as the same flag, so a bound
	// --mode=caller displaces a reviewed -mode=pinned exactly as it would
	// displace --mode=pinned. Three of the eighteen reviewed tools parse
	// their argv with that package.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "-mode=pinned")
	err := ValidateReviewedTask(task)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal for the single-dash form, got %v", err)
	}
	for _, want := range []string{"rewrite", `"mode"`, "rewrite-path", "-mode=pinned"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal must name %q, got %v", want, err)
		}
	}
}

func TestABareSingleDashReviewedFlagCollidesTheSameWay(t *testing.T) {
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "-mode")
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("expected a catalog refusal for the bare single-dash form, got %v", err)
	}
}

func TestTheRefusalNamesTheFlagAsBindingWouldSpellIt(t *testing.T) {
	// Whatever spelling the reviewed argument used, the argument that
	// displaces it is the one BindArguments emits, so the refusal says
	// --mode rather than -mode however the collision was written.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "-mode=pinned")
	err := ValidateReviewedTask(task)
	if err == nil {
		t.Fatal("expected a refusal")
	}
	if !strings.Contains(err.Error(), "second --mode") {
		t.Fatalf("refusal must name the bound spelling, got %v", err)
	}
}

func TestASingleDashPrefixOfADeclaredFlagDoesNotCollide(t *testing.T) {
	// The single-dash spelling is read by NAME like the other one, not by
	// prefix: -mode-file is a different flag from -mode.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "-mode-file=review.json")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a longer reviewed flag name must be admitted: %v", err)
	}
}

func TestASingleDashFlagTheSchemaDoesNotDeclareIsStillAdmitted(t *testing.T) {
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Positional: []string{"path"}}
	task = withReviewedArgs(task, "-checkout")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a reviewed flag the schema does not declare must be admitted: %v", err)
	}
}

func TestAnOperandNamedLikeADeclaredFlagDoesNotCollide(t *testing.T) {
	// A bare operand is not a flag however it is spelled, and refusing it
	// would be this rule reading values rather than argv.
	task := collisionTask()
	task.ArgsSchema = ArgsSchema{Flags: []string{"mode"}}
	task = withReviewedArgs(task, "mode")
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a reviewed operand must be admitted: %v", err)
	}
}
