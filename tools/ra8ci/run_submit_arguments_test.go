// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

func TestSplitSubmitTasksAttachesArgumentsToTheTaskTheyFollow(t *testing.T) {
	tasks, err := splitSubmitTasks([]string{
		"format-check",
		"run-gate", "gate=lint-go", "jobs=4",
		"test-go", "package=catalog",
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(tasks) != 3 {
		t.Fatalf("tasks = %v", tasks)
	}
	if tasks[0].Name != "format-check" || len(tasks[0].Words) != 0 {
		t.Fatalf("first task = %+v", tasks[0])
	}
	if tasks[1].Name != "run-gate" || strings.Join(tasks[1].Words, " ") != "gate=lint-go jobs=4" {
		t.Fatalf("second task = %+v", tasks[1])
	}
	if tasks[2].Name != "test-go" || strings.Join(tasks[2].Words, " ") != "package=catalog" {
		t.Fatalf("third task = %+v", tasks[2])
	}
}

// A value carrying its own '=' still attaches to the task it follows: only the
// presence of an '=' decides argument from task name, never its position.
func TestSplitSubmitTasksKeepsAValueContainingAnEquals(t *testing.T) {
	tasks, err := splitSubmitTasks([]string{"run-gate", "filter=name=value"})
	if err != nil {
		t.Fatal(err)
	}
	if len(tasks) != 1 || len(tasks[0].Words) != 1 || tasks[0].Words[0] != "filter=name=value" {
		t.Fatalf("tasks = %+v", tasks)
	}
}

func TestSplitSubmitTasksRefusesAnArgumentThatNamesNoTask(t *testing.T) {
	if _, err := splitSubmitTasks([]string{"gate=lint-go", "run-gate"}); err == nil {
		t.Fatal("a leading argument was attached to a later task")
	}
	if _, err := splitSubmitTasks(nil); err == nil {
		t.Fatal("an empty command line was accepted")
	}
}

func TestSplitSubmitTasksBoundsTheSubmission(t *testing.T) {
	words := make([]string, 0, maxSubmittedTasks+1)
	for i := 0; i <= maxSubmittedTasks; i++ {
		words = append(words, "format-check")
	}
	if _, err := splitSubmitTasks(words); err == nil {
		t.Fatalf("more than %d tasks were accepted", maxSubmittedTasks)
	}
	// The bound counts tasks, not words: arguments must not consume it.
	atCeiling := make([]string, 0, maxSubmittedTasks*2)
	for i := 0; i < maxSubmittedTasks; i++ {
		atCeiling = append(atCeiling, "run-gate", "gate=lint-go")
	}
	if _, err := splitSubmitTasks(atCeiling); err != nil {
		t.Fatalf("%d tasks with one argument each were refused: %v", maxSubmittedTasks, err)
	}
}

// The duplicate refusal is about submitting the same work twice in one run.
// Naming a task twice with different values is different work, and each entry
// already gets its own task key, so only the pair may repeat-refuse.
func TestSubmissionIdentitySeparatesTheSameTaskWithDifferentArguments(t *testing.T) {
	first := submissionIdentity("run-gate", map[string]string{"gate": "lint-go"})
	second := submissionIdentity("run-gate", map[string]string{"gate": "test-go"})
	if first == second {
		t.Fatal("two argument sets collapsed to one identity")
	}
	repeat := submissionIdentity("run-gate", map[string]string{"gate": "lint-go"})
	if first != repeat {
		t.Fatal("the same task and values did not collapse")
	}
	// Map order must not decide identity.
	ordered := submissionIdentity("run-gate", map[string]string{"gate": "lint-go", "jobs": "4"})
	reversed := submissionIdentity("run-gate", map[string]string{"jobs": "4", "gate": "lint-go"})
	if ordered != reversed {
		t.Fatal("identity depended on map iteration order")
	}
	if plain := submissionIdentity("format-check", nil); plain != "format-check" {
		t.Fatalf("argument-free identity = %q", plain)
	}
	// An argument-free task must not collide with one whose single value
	// happens to spell the separator-joined form.
	if submissionIdentity("format-check", nil) == submissionIdentity("format", map[string]string{"check": ""}) {
		t.Fatal("identities collided across the separator")
	}
}
