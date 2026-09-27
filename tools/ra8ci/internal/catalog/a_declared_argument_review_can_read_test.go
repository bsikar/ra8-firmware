// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// readableTask is the smallest task ValidateReviewedTask admits, so each test
// below changes exactly the one thing it is about.
func readableTask() Task {
	return Task{
		Name:            "readable-args",
		Version:         1,
		Tier:            "required",
		Scope:           "safe-local-read-only",
		OS:              []string{"linux"},
		DeadlineSeconds: 60,
		BoardPolicy:     "none",
		Retry:           RetryPolicy{MaxAttempts: 1},
		Steps: []Step{{
			Name:    "readable-args",
			Program: "bash",
			Args:    []string{"scripts/ci.sh", "--gate", "readable-args"},
		}},
	}
}

func TestAStepDeclaringOrdinaryArgumentsIsAdmitted(t *testing.T) {
	if err := ValidateReviewedTask(readableTask()); err != nil {
		t.Fatalf("an ordinary reviewed step was refused: %v", err)
	}
}

func TestAnArgumentCarryingANewlineIsRefused(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args[1] = "--gate\nrm -rf /"
	err := ValidateReviewedTask(task)
	if !errors.Is(err, errUnreadableDeclaredArgument) {
		t.Fatalf("a newline in a declared argument was admitted: %v", err)
	}
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the refusal did not read as an invalid catalog: %v", err)
	}
}

func TestAnArgumentCarryingAControlCharacterIsRefused(t *testing.T) {
	for name, value := range map[string]string{
		"carriage return": "--gate\rreadable",
		"tab":             "--gate\treadable",
		"escape":          "--gate\x1b[2J",
		"DEL":             "--gate\x7f",
		"C1":              "--gate\u0085",
	} {
		task := readableTask()
		task.Steps[0].Args[1] = value
		if err := ValidateReviewedTask(task); !errors.Is(err, errUnreadableDeclaredArgument) {
			t.Fatalf("a declared argument carrying a %s was admitted: %v", name, err)
		}
	}
}

// The NUL is the one byte ValidateTask already refuses in a declared argument,
// and it keeps refusing it first. This door widens that line to the rest of the
// control characters; it does not take the NUL over.
func TestANULArgumentIsStillRefusedByTheRuntimeRule(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args[1] = "--gate\x00readable"
	if err := ValidateTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the runtime rule stopped refusing a NUL argument: %v", err)
	}
	if err := ValidateReviewedTask(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a NUL argument was admitted: %v", err)
	}
}

func TestAnArgumentThatIsNotUTF8IsRefused(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args[1] = "--gate\xff\xfe"
	if err := ValidateReviewedTask(task); !errors.Is(err, errUnreadableDeclaredArgument) {
		t.Fatalf("a declared argument that is not UTF-8 was admitted: %v", err)
	}
}

func TestAnEmptyDeclaredArgumentIsRefused(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args[1] = ""
	if err := ValidateReviewedTask(task); !errors.Is(err, errUnreadableDeclaredArgument) {
		t.Fatalf("an empty declared argument was admitted: %v", err)
	}
}

func TestTheWidestDeclarableArgumentIsAdmittedAndOneByteMoreIsNot(t *testing.T) {
	task := readableTask()
	// A script that states no argument contract, because this test is
	// about how WIDE a declared argument may be, not about what any one
	// script parses. See a_script_option_the_script_parses.go.
	task.Steps[0].Args[0] = unstatedScript
	task.Steps[0].Args[1] = strings.Repeat("a", maxDeclaredArgumentBytes)
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("an argument of exactly %d bytes was refused: %v", maxDeclaredArgumentBytes, err)
	}
	task.Steps[0].Args[1] = strings.Repeat("a", maxDeclaredArgumentBytes+1)
	if err := ValidateReviewedTask(task); !errors.Is(err, errUnreadableDeclaredArgument) {
		t.Fatalf("an argument of %d bytes was admitted: %v", maxDeclaredArgumentBytes+1, err)
	}
}

func TestTheWidestDeclarableArgvIsAdmittedAndOneMoreIsNot(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args = make([]string, maxDeclaredArgumentsPerStep)
	for i := range task.Steps[0].Args {
		task.Steps[0].Args[i] = unstatedScript
	}
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a step declaring exactly %d arguments was refused: %v", maxDeclaredArgumentsPerStep, err)
	}
	task.Steps[0].Args = append(task.Steps[0].Args, unstatedScript)
	if err := ValidateReviewedTask(task); !errors.Is(err, errUnreadableDeclaredArgument) {
		t.Fatalf("a step declaring %d arguments was admitted: %v", maxDeclaredArgumentsPerStep+1, err)
	}
}

// The two rules this door deliberately does NOT borrow from ValidArgumentValue,
// which judges what a CALLER supplies. A declared literal is reviewed argv and
// nothing re-splits it, so both shapes below are ordinary and most of the tree
// declares the first.
func TestTheDeclaredArgumentDoorDoesNotRefuseALeadingDash(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args[1] = "--gate"
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a declared flag was refused: %v", err)
	}
}

func TestTheDeclaredArgumentDoorDoesNotChooseAnAlphabet(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args[0] = unstatedScript
	task.Steps[0].Args[1] = "--pattern=*.zig"
	if err := ValidateReviewedTask(task); err != nil {
		t.Fatalf("a declared argument carrying a glob was refused: %v", err)
	}
}

// The runtime re-check is deliberately untouched: an agent holding a task it
// was granted against a reviewed digest must not have it refused under a rule
// admission added later.
func TestTheRuntimeRecheckStillAdmitsWhatReviewNowRefuses(t *testing.T) {
	task := readableTask()
	task.Steps[0].Args[1] = "--gate\nrm -rf /"
	if err := ValidateTask(task); err != nil {
		t.Fatalf("the runtime re-check refused a task it already holds: %v", err)
	}
}

// The embedded catalog is the tree this rule has to keep admitting.
func TestTheEmbeddedCatalogDeclaresOnlyArgumentsReviewCanRead(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("the embedded catalog no longer loads: %v", err)
	}
	for _, name := range loaded.Names() {
		task, ok := loaded.Task(name)
		if !ok {
			t.Fatalf("catalog names %q but does not carry it", name)
		}
		if err := checkEachDeclaredArgumentIsOneReviewCanRead(task); err != nil {
			t.Fatalf("embedded task %q: %v", name, err)
		}
	}
}
