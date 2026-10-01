// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

func loadedCatalog(t *testing.T) *catalog.Catalog {
	t.Helper()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatalf("load embedded catalog: %v", err)
	}
	return definitions
}

// The bare verb is what scripts already parse, so it keeps printing names and
// only names, one per line, in manifest order.
func TestTasksWithNoOptionStillPrintsOnlyNames(t *testing.T) {
	definitions := loadedCatalog(t)
	var out bytes.Buffer
	if err := tasksCommand(&out, definitions, nil); err != nil {
		t.Fatalf("tasks: %v", err)
	}
	lines := strings.Split(strings.TrimSuffix(out.String(), "\n"), "\n")
	names := definitions.Names()
	if len(lines) != len(names) {
		t.Fatalf("printed %d lines for %d tasks", len(lines), len(names))
	}
	for i, name := range names {
		if lines[i] != name {
			t.Fatalf("line %d is %q, want %q", i, lines[i], name)
		}
	}
}

// The digest is the whole point of the verb: one line, nothing around it, so a
// person reconciling an attempt row can compare it without editing the output.
func TestTasksDigestPrintsTheDigestAlone(t *testing.T) {
	definitions := loadedCatalog(t)
	var out bytes.Buffer
	if err := tasksCommand(&out, definitions, []string{"--digest"}); err != nil {
		t.Fatalf("tasks --digest: %v", err)
	}
	if out.String() != definitions.Digest()+"\n" {
		t.Fatalf("digest output %q, want %q", out.String(), definitions.Digest()+"\n")
	}
	if definitions.Digest() == "" {
		t.Fatal("embedded catalog reported an empty digest")
	}
}

// The document carries the digest beside the definitions, because a definition
// quoted without one cannot be tied to the row that names it.
func TestTasksJSONCarriesEveryDefinitionAndTheDigest(t *testing.T) {
	definitions := loadedCatalog(t)
	var out bytes.Buffer
	if err := tasksCommand(&out, definitions, []string{"--json"}); err != nil {
		t.Fatalf("tasks --json: %v", err)
	}
	dec := json.NewDecoder(bytes.NewReader(out.Bytes()))
	dec.DisallowUnknownFields()
	var report catalogReport
	if err := dec.Decode(&report); err != nil {
		t.Fatalf("decode report: %v", err)
	}
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		t.Fatalf("expected exactly one document, got %v", err)
	}
	if report.SchemaVersion != catalog.SchemaVersion {
		t.Fatalf("schema version %d, want %d", report.SchemaVersion, catalog.SchemaVersion)
	}
	if report.Digest != definitions.Digest() {
		t.Fatalf("digest %q, want %q", report.Digest, definitions.Digest())
	}
	names := definitions.Names()
	if len(report.Tasks) != len(names) {
		t.Fatalf("reported %d tasks, catalog holds %d", len(report.Tasks), len(names))
	}
	for i, name := range names {
		if report.Tasks[i].Name != name {
			t.Fatalf("task %d is %q, want %q (manifest order)", i, report.Tasks[i].Name, name)
		}
	}
}

// A definition is only worth printing if the argv survives it: the steps are
// the part a reader is checking against what actually ran.
func TestTasksJSONPreservesStepArgv(t *testing.T) {
	definitions := loadedCatalog(t)
	var out bytes.Buffer
	if err := tasksCommand(&out, definitions, []string{"--json"}); err != nil {
		t.Fatalf("tasks --json: %v", err)
	}
	var report catalogReport
	if err := json.Unmarshal(out.Bytes(), &report); err != nil {
		t.Fatalf("decode report: %v", err)
	}
	steps := 0
	for _, reported := range report.Tasks {
		source, found := definitions.Task(reported.Name)
		if !found {
			t.Fatalf("reported task %q is not in the catalog", reported.Name)
		}
		if reported.Scope != source.Scope || reported.DeadlineSeconds != source.DeadlineSeconds ||
			reported.BoardPolicy != source.BoardPolicy || reported.Tier != source.Tier {
			t.Fatalf("task %q lost its contract in the report", reported.Name)
		}
		if len(reported.Steps) != len(source.Steps) {
			t.Fatalf("task %q reported %d steps, has %d", reported.Name, len(reported.Steps), len(source.Steps))
		}
		for i, step := range source.Steps {
			steps++
			if reported.Steps[i].Program != step.Program || reported.Steps[i].Name != step.Name {
				t.Fatalf("task %q step %d lost its program", reported.Name, i)
			}
			if len(reported.Steps[i].Args) != len(step.Args) {
				t.Fatalf("task %q step %d lost arguments", reported.Name, i)
			}
			for j, arg := range step.Args {
				if reported.Steps[i].Args[j] != arg {
					t.Fatalf("task %q step %d argument %d is %q, want %q", reported.Name, i, j, reported.Steps[i].Args[j], arg)
				}
			}
		}
	}
	if steps == 0 {
		t.Fatal("no steps compared, the embedded catalog reported none")
	}
}

// A refusal is decided before anything is written, so a misuse never leaves a
// partial name list or half a document on the stream for a script to read.
func TestTasksRefusesMisuseWithoutWritingAnything(t *testing.T) {
	definitions := loadedCatalog(t)
	for _, args := range [][]string{
		{"--jsonn"},
		{"--all"},
		{"-json"},
		{""},
		{"lint"},
		{"--json", "--digest"},
		{"--digest", "extra"},
	} {
		var out bytes.Buffer
		err := tasksCommand(&out, definitions, args)
		if !errors.Is(err, errTasksUsage) {
			t.Fatalf("tasks %v returned %v, want a usage refusal", args, err)
		}
		if out.Len() != 0 {
			t.Fatalf("tasks %v wrote %q before refusing", args, out.String())
		}
	}
}

// Misuse is refused before the catalog is even consulted, which is what lets
// the caller report it as misuse rather than as a failure to read the catalog.
func TestTasksRefusesMisuseAheadOfTheCatalog(t *testing.T) {
	var out bytes.Buffer
	if err := tasksCommand(&out, nil, []string{"--nope"}); !errors.Is(err, errTasksUsage) {
		t.Fatalf("returned %v, want a usage refusal", err)
	}
	err := tasksCommand(&out, nil, nil)
	if err == nil || errors.Is(err, errTasksUsage) {
		t.Fatalf("a missing catalog returned %v, want a plain failure", err)
	}
	if out.Len() != 0 {
		t.Fatalf("wrote %q with no catalog", out.String())
	}
}

// A stream that stops accepting bytes is reported, never swallowed: the caller
// exits non-zero rather than implying the catalog was printed.
func TestTasksReportsAWriteFailure(t *testing.T) {
	definitions := loadedCatalog(t)
	for _, args := range [][]string{nil, {"--digest"}, {"--json"}} {
		if err := tasksCommand(refusingWriter{}, definitions, args); err == nil {
			t.Fatalf("tasks %v swallowed a write failure", args)
		}
	}
}

type refusingWriter struct{}

func (refusingWriter) Write([]byte) (int, error) { return 0, errors.New("stream closed") }
