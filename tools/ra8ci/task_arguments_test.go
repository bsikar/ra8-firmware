// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func schemaTask() catalog.Task {
	return catalog.Task{
		Name:       "fixture",
		ArgsSchema: catalog.ArgsSchema{Positional: []string{"gate"}, Flags: []string{"jobs"}},
	}
}

func TestParseTaskArgumentsReadsNamedValues(t *testing.T) {
	values, err := parseTaskArguments([]string{"gate=lint-go", "jobs=4"})
	if err != nil {
		t.Fatal(err)
	}
	if len(values) != 2 || values["gate"] != "lint-go" || values["jobs"] != "4" {
		t.Fatalf("values = %v", values)
	}
	// The first '=' separates, so a value may carry one.
	values, err = parseTaskArguments([]string{"gate=a=b"})
	if err != nil || values["gate"] != "a=b" {
		t.Fatalf("values = %v, err = %v", values, err)
	}
}

func TestParseTaskArgumentsRefusesWhatCannotBeBoundHonestly(t *testing.T) {
	for name, words := range map[string][]string{
		"bare word":   {"lint-go"},
		"empty name":  {"=value"},
		"bad name":    {"--gate=lint-go"},
		"empty value": {"gate="},
		"metachar":    {"gate=lint;rm -rf /"},
		"repeated":    {"gate=a", "gate=b"},
	} {
		if _, err := parseTaskArguments(words); err == nil {
			t.Fatalf("%s: %q was accepted", name, words)
		}
	}
	tooMany := make([]string, maxCommandLineArguments+1)
	for i := range tooMany {
		tooMany[i] = "gate=lint-go"
	}
	if _, err := parseTaskArguments(tooMany); err == nil {
		t.Fatal("an unbounded argument list was accepted")
	}
}

// Every task in the v1 catalog declares no arguments, and their refusal must
// stay the plain one rather than a complaint about name=value shape.
func TestTaskArgumentValuesKeepsThePlainRefusalForArgumentlessTasks(t *testing.T) {
	task := catalog.Task{Name: "format-check"}
	values, err := taskArgumentValues(task, nil)
	if err != nil || values != nil {
		t.Fatalf("values = %v, err = %v", values, err)
	}
	err = errorFrom(taskArgumentValues(task, []string{"extra"}))
	if err == nil || !strings.Contains(err.Error(), "accepts no arguments") {
		t.Fatalf("err = %v, want the argumentless refusal", err)
	}
}

func TestTaskArgumentValuesNamesWhatTheTaskAccepts(t *testing.T) {
	err := errorFrom(taskArgumentValues(schemaTask(), []string{"lint-go"}))
	if err == nil {
		t.Fatal("a bare word was accepted for a task with a schema")
	}
	if !strings.Contains(err.Error(), "gate, jobs") {
		t.Fatalf("refusal did not name the accepted arguments: %v", err)
	}
	values, bindErr := taskArgumentValues(schemaTask(), []string{"gate=lint-go", "jobs=4"})
	if bindErr != nil || values["gate"] != "lint-go" {
		t.Fatalf("values = %v, err = %v", values, bindErr)
	}
}

func TestArgumentUsageNamesNothingForAnArgumentlessTask(t *testing.T) {
	if got := argumentUsage(catalog.Task{Name: "format-check"}); got != "no arguments" {
		t.Fatalf("argumentUsage() = %q", got)
	}
}

func errorFrom(_ map[string]string, err error) error { return err }
