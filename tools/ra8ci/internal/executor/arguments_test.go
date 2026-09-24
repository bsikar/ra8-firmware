// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func gateStep() catalog.Step {
	return catalog.Step{Name: "gate", Program: "bash", Args: []string{"scripts/ci.sh", "--gate", "lint-go"}}
}

func TestBoundStepAppendsValuesAfterTheReviewedArguments(t *testing.T) {
	step := gateStep()
	dispatch, err := boundStep(step, []string{"release", "--jobs=4"})
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"scripts/ci.sh", "--gate", "lint-go", "release", "--jobs=4"}
	if strings.Join(dispatch.Args, "\x1f") != strings.Join(want, "\x1f") {
		t.Fatalf("argv = %q, want %q", dispatch.Args, want)
	}
	if dispatch.Program != "bash" || dispatch.Name != "gate" {
		t.Fatalf("dispatch changed the step identity: %+v", dispatch)
	}
}

// A bound value must not be able to reach back into the reviewed definition it
// was bound against: the next attempt of the same task reads those arguments.
func TestBoundStepDoesNotMutateTheReviewedStep(t *testing.T) {
	step := gateStep()
	reviewed := make([]string, len(step.Args))
	copy(reviewed, step.Args)
	dispatch, err := boundStep(step, []string{"bound"})
	if err != nil {
		t.Fatal(err)
	}
	dispatch.Args[0] = "scripts/other.sh"
	if strings.Join(step.Args, "\x1f") != strings.Join(reviewed, "\x1f") {
		t.Fatalf("reviewed args mutated through the dispatch: %q", step.Args)
	}
}

// The dispatch seam (#1517) is that a step is either an ra8ci tool or bash
// whose first argument is a reviewed script path. Binding appends, so it
// cannot displace either, whatever a caller supplies.
func TestBoundStepCannotDisplaceTheReviewedDispatch(t *testing.T) {
	for _, bound := range [][]string{
		{"-c"},
		{"scripts/attacker.sh"},
		{"--gate=other"},
	} {
		dispatch, err := boundStep(gateStep(), bound)
		if err != nil {
			t.Fatalf("bound %q: %v", bound, err)
		}
		if dispatch.Args[0] != "scripts/ci.sh" {
			t.Fatalf("bound %q moved the script path: %q", bound, dispatch.Args)
		}
		if err := catalog.ValidateStepDispatch(dispatch); err != nil {
			t.Fatalf("bound %q broke the dispatch seam: %v", bound, err)
		}
	}
}

func TestBoundStepRefusesArgumentsNoProcessShouldReceive(t *testing.T) {
	for name, bound := range map[string][]string{
		"empty":   {""},
		"nul":     {"a\x00b"},
		"newline": {"a\nb"},
		"return":  {"a\rb"},
	} {
		if _, err := boundStep(gateStep(), bound); !errors.Is(err, ErrUnreviewedTask) {
			t.Fatalf("%s: err = %v, want ErrUnreviewedTask", name, err)
		}
	}
	reviewed := catalog.Step{Name: "gate", Program: "bash", Args: []string{"scripts/ci.sh\x00"}}
	if _, err := boundStep(reviewed, nil); !errors.Is(err, ErrUnreviewedTask) {
		t.Fatalf("NUL in a reviewed argument: err = %v, want ErrUnreviewedTask", err)
	}
}

// The whole point of the wiring: a bound value reaches the process.
func TestRunBoundTaskCarriesBoundArgumentsToTheProcess(t *testing.T) {
	task := catalog.Task{
		Name: "fixture", Version: 1, Tier: "required", Scope: "safe-local-read-only",
		OS: []string{runtime.GOOS}, DeadlineSeconds: 3, BoardPolicy: "none",
		Retry: catalog.RetryPolicy{MaxAttempts: 1},
		Steps: []catalog.Step{{Name: "echo-args", Program: os.Args[0], Args: helperArgs("echo-args")}},
	}
	var stdout bytes.Buffer
	result, err := runBoundTask(context.Background(), t.TempDir(), task, []string{"first", "--flag=second"},
		func(string) (io.Writer, io.Writer) { return &stdout, io.Discard }, time.Millisecond)
	if err != nil || result.ExitCode != 0 {
		t.Fatalf("result = %+v, err = %v", result, err)
	}
	if stdout.String() != "first\n--flag=second\n" {
		t.Fatalf("process argv = %q", stdout.String())
	}
}

// Every task in the v1 catalog declares no arguments, so the bound path is a
// no-op for them and a caller supplying values is refused before anything runs.
func TestBindArgumentsRefusesValuesForAnArgumentlessTask(t *testing.T) {
	task := catalog.Task{Name: "fixture"}
	if _, err := task.BindArguments(map[string]string{"gate": "lint-go"}); err == nil {
		t.Fatal("a task that declares no arguments accepted values")
	}
	bound, err := task.BindArguments(nil)
	if err != nil || len(bound) != 0 {
		t.Fatalf("bound = %q, err = %v", bound, err)
	}
}
