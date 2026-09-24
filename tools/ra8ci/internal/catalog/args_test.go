// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func TestValidArgumentName(t *testing.T) {
	for _, name := range []string{"target", "board-id", "profile2"} {
		if !ValidArgumentName(name) {
			t.Fatalf("declared name %q refused", name)
		}
	}
	for _, name := range []string{"", "-target", "2target", "target-", "tar--get",
		"Target", "tar_get", "tar.get", "tar get", strings.Repeat("a", MaxArgumentNameBytes+1)} {
		if ValidArgumentName(name) {
			t.Fatalf("unusable name %q accepted", name)
		}
	}
}

// TestValidArgumentValueRefusesEverythingAShellWouldRead is the value half of
// the contract. argv is what makes the path safe; this is the allowlist that
// keeps a caller's mistake from reaching it.
func TestValidArgumentValueRefusesEverythingAShellWouldRead(t *testing.T) {
	for _, value := range []string{"examples/ra8p1/hil.conf", "ra8p1", "release", "1", "a+b", "v1.2.3", "a,b", "a=b", "a:b", "a@b", "a/b"} {
		if !ValidArgumentValue(value) {
			t.Fatalf("ordinary value %q refused", value)
		}
	}
	refused := []string{
		"", " ", "trailing ", " leading", "--check", "-x",
		"a;rm -rf /", "a && b", "a | b", "a > out", "a < in", "a `id`", "a $(id)",
		"a $HOME", "a\\b", "a\"b", "a'b", "a*b", "a?b", "a[b]", "a{b}", "a!b", "a~b",
		"a^b", "a%b", "a\tb", "a\nb", "a\rb", "a\x00b", "a\x1bb", "héllo",
		strings.Repeat("a", MaxArgumentValueBytes+1),
	}
	for _, value := range refused {
		if ValidArgumentValue(value) {
			t.Fatalf("value %q accepted", value)
		}
	}
}

func TestValidateArgsSchemaShape(t *testing.T) {
	if err := ValidateArgsSchema(ArgsSchema{}); err != nil {
		t.Fatalf("empty schema refused: %v", err)
	}
	if err := ValidateArgsSchema(ArgsSchema{Positional: []string{"target"}, Flags: []string{"profile"}}); err != nil {
		t.Fatalf("valid schema refused: %v", err)
	}
	for name, schema := range map[string]ArgsSchema{
		"invalid positional name": {Positional: []string{"Target"}},
		"invalid flag name":       {Flags: []string{"-profile"}},
		"positional twice":        {Positional: []string{"target", "target"}},
		"flag twice":              {Flags: []string{"profile", "profile"}},
		// One name in both groups would make a supplied value ambiguous.
		"both groups":       {Positional: []string{"target"}, Flags: []string{"target"}},
		"too many position": {Positional: make([]string, MaxPositionalArguments+1)},
		"too many flags":    {Flags: make([]string, MaxFlagArguments+1)},
	} {
		if err := ValidateArgsSchema(schema); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s accepted: %v", name, err)
		}
	}
}

func TestBindArgumentsProducesArgvInDeclaredOrder(t *testing.T) {
	schema := ArgsSchema{Positional: []string{"target", "board-id"}, Flags: []string{"profile", "mode"}}
	argv, err := BindArguments(schema, map[string]string{
		"target": "ra8p1", "board-id": "bench-1", "mode": "alive"})
	if err != nil {
		t.Fatalf("bind = %v", err)
	}
	// Positionals in declared order, then the one flag supplied. The absent
	// flag is absent, not empty, and every argument is exactly one element.
	want := []string{"ra8p1", "bench-1", "--mode=alive"}
	if len(argv) != len(want) {
		t.Fatalf("argv = %q, want %q", argv, want)
	}
	for index := range want {
		if argv[index] != want[index] {
			t.Fatalf("argv = %q, want %q", argv, want)
		}
	}
}

func TestBindArgumentsRefusesWhatItCannotRunFaithfully(t *testing.T) {
	schema := ArgsSchema{Positional: []string{"target"}, Flags: []string{"profile"}}
	for name, values := range map[string]map[string]string{
		"undeclared name":    {"target": "ra8p1", "sudo": "yes"},
		"missing positional": {"profile": "release"},
		"positional value":   {"target": "a; rm -rf /"},
		"flag value":         {"target": "ra8p1", "profile": "--x"},
		"empty flag value":   {"target": "ra8p1", "profile": ""},
	} {
		if _, err := BindArguments(schema, values); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s accepted: %v", name, err)
		}
	}
	// The refusal names what the task does accept, so a caller can fix it.
	_, err := BindArguments(schema, map[string]string{"target": "ra8p1", "sudo": "yes"})
	if err == nil || !strings.Contains(err.Error(), "sudo") {
		t.Fatalf("undeclared argument not named: %v", err)
	}
}

// TestTaskBindArgumentsKeepsTheV1Refusal pins that stating this contract has
// not quietly opened arguments to the embedded catalog: every reviewed task
// declares no schema today, and ValidateTask still refuses one that does.
func TestTaskBindArgumentsKeepsTheV1Refusal(t *testing.T) {
	definitions, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range definitions.Names() {
		task, found := definitions.Task(name)
		if !found {
			t.Fatalf("catalog names %q but does not hold it", name)
		}
		if len(task.ArgsSchema.Positional) != 0 || len(task.ArgsSchema.Flags) != 0 {
			t.Fatalf("task %q declares arguments the v1 catalog cannot carry", name)
		}
		argv, err := task.BindArguments(nil)
		if err != nil || len(argv) != 0 {
			t.Fatalf("task %q with no arguments = %q, %v", name, argv, err)
		}
		if _, err := task.BindArguments(map[string]string{"target": "ra8p1"}); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("task %q accepted an argument it does not declare: %v", name, err)
		}
	}
	// A schema on a definition is still refused at review time.
	schemaTask := Task{Name: "format-check", Version: 1, Tier: "required",
		Scope: "safe-local-read-only", OS: []string{"linux"}, DeadlineSeconds: 900,
		BoardPolicy: "none", Retry: RetryPolicy{MaxAttempts: 1},
		ArgsSchema: ArgsSchema{Flags: []string{"profile"}},
		Steps:      []Step{{Name: "check", Program: "bash", Args: []string{"x.sh"}}}}
	if err := ValidateTask(schemaTask); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a v1 definition declaring arguments was accepted: %v", err)
	}
	// And a task that does declare one binds against it.
	argv, err := schemaTask.BindArguments(map[string]string{"profile": "release"})
	if err != nil || len(argv) != 1 || argv[0] != "--profile=release" {
		t.Fatalf("declared schema bind = %q, %v", argv, err)
	}
}

func TestStepArgvAppendsBoundArgumentsAfterTheReviewedOnes(t *testing.T) {
	step := Step{Name: "check", Program: "bash", Args: []string{"scripts/checks/format_tree.sh", "--check"}}
	argv, err := StepArgv(step, []string{"ra8p1", "--mode=alive"})
	if err != nil {
		t.Fatalf("argv = %v", err)
	}
	want := []string{"scripts/checks/format_tree.sh", "--check", "ra8p1", "--mode=alive"}
	if strings.Join(argv, "\x00") != strings.Join(want, "\x00") {
		t.Fatalf("argv = %q, want %q", argv, want)
	}
	// The result is a copy: writing through it must not reach the catalog.
	argv[0] = "rewritten"
	if step.Args[0] != "scripts/checks/format_tree.sh" {
		t.Fatalf("reviewed definition was mutated through the argv: %q", step.Args)
	}
	for name, bound := range map[string][]string{
		"empty":   {""},
		"NUL":     {"a\x00b"},
		"newline": {"a\nb"},
	} {
		if _, err := StepArgv(step, bound); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s bound argument accepted: %v", name, err)
		}
	}
	if _, err := StepArgv(Step{Args: []string{"a\x00b"}}, nil); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatal("NUL in a reviewed argument accepted")
	}
}
