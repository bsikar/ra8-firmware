// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// The Zig half of the task contract, stated where every other dispatch rule is
// stated. `zig build` is a front door: it reads build.zig out of the checkout
// and decides from there what to compile and with what flags, so a step naming
// it would pin the word and nothing about the work a reviewed digest is
// supposed to hold. The compiler is reached inside a reviewed script instead,
// the same way every C gate reaches its own compiler.
func TestZigIsRefusedAsADispatchedProgram(t *testing.T) {
	if !IsFrontDoorProgram("zig") {
		t.Fatal("zig is not held as a front door, so a reviewed step could dispatch it directly")
	}

	for _, args := range [][]string{
		{"build"},
		{"build", "-Doptimize=ReleaseSafe"},
		{"build", "test"},
		{"fmt", "--check", "."},
		{"test", "src/main.zig"},
	} {
		step := Step{Name: "zig-build", Program: "zig", Args: args}
		err := ValidateStepDispatch(step)
		if !errors.Is(err, ErrFrontDoorProgram) {
			t.Fatalf("zig %v: want ErrFrontDoorProgram, got %v", args, err)
		}
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("zig %v: refusal must stay an invalid-catalog error, got %v", args, err)
		}
		if !strings.Contains(err.Error(), "zig") {
			t.Fatalf("refusal must name the program, got %v", err)
		}
	}
}

// The refusal names the front door rather than only saying the program was not
// bash, which is the whole reason the list exists: a reviewer reading it should
// learn where the compiler belongs.
func TestTheZigRefusalSaysWhereTheCompilerBelongs(t *testing.T) {
	err := ValidateStepDispatch(Step{Name: "zig-build", Program: "zig", Args: []string{"build"}})
	if err == nil {
		t.Fatal("a step dispatching zig was admitted")
	}
	if !strings.Contains(err.Error(), "dispatch its reviewed script instead") {
		t.Fatalf("refusal does not point at the reviewed script: %v", err)
	}
	if strings.Contains(err.Error(), "not \"bash\"") {
		t.Fatalf("refusal fell through to the generic program arm: %v", err)
	}
}

// A Zig build reached through a reviewed script is the shape that IS admitted,
// so the rule above refuses the front door without closing the door the
// contract actually offers. The script path is judged on its shape here; which
// scripts the checkout ships is the reviewed list's own rule.
func TestAScriptIsStillTheWayAZigBuildIsDispatched(t *testing.T) {
	if !ValidScriptPath("scripts/checks/zig_build.sh") {
		t.Fatal("a reviewed zig build script is not a valid dispatch target by shape")
	}
	if IsFrontDoorProgram(DispatchShell) {
		t.Fatalf("%q is held as a front door, which would refuse every reviewed script", DispatchShell)
	}
}
