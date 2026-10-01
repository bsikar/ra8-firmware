// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"testing"
)

// argvRecheckTask is a task with both halves of a reviewed schema declared.
func argvRecheckTask() Task {
	return Task{
		Name:       "argv-recheck",
		ArgsSchema: ArgsSchema{Positional: []string{"path"}, Flags: []string{"mode", "limit"}},
	}
}

func TestTaskWithNoSchemaStillAcceptsNoArguments(t *testing.T) {
	task := Task{Name: "closed"}
	if err := task.ValidateArguments(nil); err != nil {
		t.Fatalf("empty argv refused: %v", err)
	}
	err := task.ValidateArguments([]string{"extra"})
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("argv for a task with no schema was accepted: %v", err)
	}
}

func TestArgvBoundFromTheSchemaIsAccepted(t *testing.T) {
	task := argvRecheckTask()
	for _, values := range []map[string]string{
		{"path": "tools/ra8ci/main.go"},
		{"path": "tools/ra8ci/main.go", "limit": "8"},
		{"path": "tools/ra8ci/main.go", "mode": "check", "limit": "8"},
	} {
		argv, err := task.BindArguments(values)
		if err != nil {
			t.Fatalf("binding %v failed: %v", values, err)
		}
		if err := task.ValidateArguments(argv); err != nil {
			t.Fatalf("re-check refused argv %v its own binding produced: %v", argv, err)
		}
	}
}

func TestArgvMissingAPositionalIsRefused(t *testing.T) {
	err := argvRecheckTask().ValidateArguments(nil)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("argv with no positional was accepted: %v", err)
	}
}

func TestArgvNamingAnUndeclaredFlagIsRefused(t *testing.T) {
	err := argvRecheckTask().ValidateArguments([]string{"src", "--depth=3"})
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("undeclared flag was accepted: %v", err)
	}
}

func TestArgvRepeatingAFlagIsRefused(t *testing.T) {
	err := argvRecheckTask().ValidateArguments([]string{"src", "--mode=check", "--mode=fix"})
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("repeated flag was accepted: %v", err)
	}
}

func TestArgvOutOfDeclaredFlagOrderIsRefused(t *testing.T) {
	err := argvRecheckTask().ValidateArguments([]string{"src", "--limit=8", "--mode=check"})
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("flags out of declared order were accepted: %v", err)
	}
}

func TestArgvElementThatIsNotABoundFlagIsRefused(t *testing.T) {
	for _, element := range []string{"mode=check", "--mode", "-mode=check", "--=check"} {
		err := argvRecheckTask().ValidateArguments([]string{"src", element})
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("element %q was accepted: %v", element, err)
		}
	}
}

func TestArgvCarryingAnUnsafeValueIsRefused(t *testing.T) {
	task := argvRecheckTask()
	for _, argv := range [][]string{
		{"src; rm -rf /"},
		{"src\n"},
		{"-flagish"},
		{""},
		{"src", "--mode=check`id`"},
		{"src", "--mode="},
	} {
		if err := task.ValidateArguments(argv); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("argv %v was accepted: %v", argv, err)
		}
	}
}

func TestArgvIsRefusedWhenTheSchemaItselfIsInvalid(t *testing.T) {
	task := Task{Name: "broken", ArgsSchema: ArgsSchema{Positional: []string{"path"}, Flags: []string{"path"}}}
	if err := task.ValidateArguments([]string{"src"}); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("argv against a self-contradictory schema was accepted: %v", err)
	}
}
