// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// The run subcommands all end in the same place: a client built from the
// operator's mTLS material, a call over it, and JSON on stdout. Everything
// this file pins happens strictly before that, on the arguments alone, which
// is the half a box with no plane can judge and the half a typing mistake
// actually meets. A refusal here costs the operator nothing; the same mistake
// carried past the argument check spends a connection and comes back as an
// invalid_argument from the API, naming the field rather than the word they
// typed.

// refusedRun runs one run subcommand and demands a refusal from it. A
// subcommand that got as far as building a client has not refused on its
// arguments, and the case says so rather than passing on an error that came
// from somewhere else entirely.
func refusedRun(t *testing.T, run func(context.Context, []string) error, args ...string) string {
	t.Helper()
	err := run(context.Background(), args)
	if err == nil {
		t.Fatalf("args %q were accepted", args)
	}
	return err.Error()
}

func TestRunLogsRefusesAnythingButTwoIdentifiers(t *testing.T) {
	const usage = "usage: ra8ci run logs [--after SEQ] [--limit 1..8] RUN_ID ATTEMPT_ID"
	for name, args := range map[string][]string{
		"nothing":                         {},
		"only the run":                    {"run-1"},
		"a third word":                    {"run-1", "attempt-1", "extra"},
		"an unknown flag":                 {"--nope", "run-1", "attempt-1"},
		"a flag wanting a value":          {"--after"},
		"a sequence that is not a number": {"--after", "soon", "run-1", "attempt-1"},
		"a limit that is not a number":    {"--limit", "some", "run-1", "attempt-1"},
	} {
		t.Run(name, func(t *testing.T) {
			if message := refusedRun(t, showRunLogs, args...); message != usage {
				t.Fatalf("refusal = %q, want the usage", message)
			}
		})
	}
}

func TestRunEventsRefusesAnythingButOneValidIdentifier(t *testing.T) {
	const usage = "usage: ra8ci run events [--after SEQ] [--limit 1..50] RUN_ID"
	for name, args := range map[string][]string{
		"nothing":                         {},
		"two runs":                        {validRunID, validRunID},
		"an unknown flag":                 {"--nope", validRunID},
		"a sequence that is not a number": {"--after", "soon", validRunID},
		// The identifier is judged here, not at the plane: a word that
		// cannot be a run id never becomes a request.
		"an identifier that is not one": {"not-a-run-id"},
		"an empty identifier":           {""},
	} {
		t.Run(name, func(t *testing.T) {
			if message := refusedRun(t, showRunEvents, args...); message != usage {
				t.Fatalf("refusal = %q, want the usage", message)
			}
		})
	}
}

func TestRunCancelRefusesAnythingButOneValidIdentifier(t *testing.T) {
	const usage = "usage: ra8ci run cancel RUN_ID"
	for name, args := range map[string][]string{
		"nothing":                       {},
		"two runs":                      {validRunID, validRunID},
		"an identifier that is not one": {"not-a-run-id"},
		"an empty identifier":           {""},
		// cancel takes no flags, so a flag is just a word that is not an
		// identifier, and it is refused as one rather than parsed.
		"a flag": {"--force"},
	} {
		t.Run(name, func(t *testing.T) {
			if message := refusedRun(t, cancelRun, args...); message != usage {
				t.Fatalf("refusal = %q, want the usage", message)
			}
		})
	}
}

// TestRunStatusRefusesAnythingButOneWord pins the one asymmetry among these
// four. status does NOT judge the identifier's shape, only the count, so a
// word that cancel refuses locally is carried to the plane here. That is a
// deliberate difference and not a missing check: reading a run is safe, and
// the plane's own answer names what it could not find.
func TestRunStatusRefusesAnythingButOneWord(t *testing.T) {
	const usage = "usage: ra8ci run status RUN_ID"
	for name, args := range map[string][]string{
		"nothing":  {},
		"two runs": {validRunID, validRunID},
	} {
		t.Run(name, func(t *testing.T) {
			if message := refusedRun(t, showRun, args...); message != usage {
				t.Fatalf("refusal = %q, want the usage", message)
			}
		})
	}
}

// TestRunSubmitRefusesBeforeItLooksForACheckout pins the refusals submit
// makes on its own words. Everything past them needs a clean pinned checkout,
// which this box does not have, so these are the whole of what a submit can
// judge without one. The key and at least one task are both required, and the
// usage is stated once in submitUsage rather than restated per refusal.
func TestRunSubmitRefusesBeforeItLooksForACheckout(t *testing.T) {
	for name, args := range map[string][]string{
		"nothing at all":         {},
		"a key and no task":      {"--idempotency-key", "k-1"},
		"a task and no key":      {"build"},
		"an empty key":           {"--idempotency-key", "", "build"},
		"an unknown flag":        {"--nope", "--idempotency-key", "k-1", "build"},
		"a flag wanting a value": {"--idempotency-key"},
	} {
		t.Run(name, func(t *testing.T) {
			message := refusedRun(t, submitRun, args...)
			if !strings.HasPrefix(message, submitUsage) {
				t.Fatalf("refusal = %q, want it to open with the submit usage", message)
			}
		})
	}
}

// TestRunSubmitRefusesAnArgumentWithoutATask pins the one grammar rule the
// submit words carry: NAME=VALUE belongs to the task in front of it, so an
// assignment with no task before it is a typing mistake rather than a task
// named "NAME=VALUE". The refusal keeps the usage so the operator sees the
// shape it was meant to take.
func TestRunSubmitRefusesAnArgumentWithoutATask(t *testing.T) {
	message := refusedRun(t, submitRun, "--idempotency-key", "k-1", "board=ek_ra8d2")
	if !strings.HasPrefix(message, submitUsage) {
		t.Fatalf("refusal = %q, want it to open with the submit usage", message)
	}
	if !strings.Contains(message, "board=ek_ra8d2") {
		t.Fatalf("refusal = %q, want it to quote the word it could not place", message)
	}
}

// TestTheRunFrontDoorRefusesNoSubcommandAtAll covers the arm its companion
// case in run_usage_test.go does not: an unknown word is answered with the
// usage there, and no word at all is answered with the same usage here.
func TestTheRunFrontDoorRefusesNoSubcommandAtAll(t *testing.T) {
	message := refusedRun(t, runCommand)
	if message != "usage: ra8ci "+runUsage() {
		t.Fatalf("refusal = %q, want the run usage", message)
	}
}

// TestRunOutputRefusesAWriterThatCannotTakeIt pins what every one of these
// subcommands does with its answer once it has one. A closed pipe is the
// ordinary way this happens: the operator pipes the run into head, head
// leaves, and the write fails. The error names the writing rather than the
// run, because the run itself was fine.
func TestRunOutputRefusesAWriterThatCannotTakeIt(t *testing.T) {
	err := writeJSON(refusingWriter{}, map[string]string{"run": validRunID})
	if err == nil {
		t.Fatal("a writer that refuses everything was reported as written to")
	}
	if !strings.HasPrefix(err.Error(), "write JSON output:") {
		t.Fatalf("error = %q, want it to name the write", err)
	}
	if !strings.Contains(err.Error(), "stream closed") {
		t.Fatalf("error = %q, want the writer's own reason carried", err)
	}
}

// TestRunOutputWritesOneJSONLinePerValue pins the shape the reader on the
// other end is promised: one value per line, so a paged listing can be read
// with a line-oriented tool as it arrives rather than only once complete.
func TestRunOutputWritesOneJSONLinePerValue(t *testing.T) {
	var written strings.Builder
	for _, id := range []string{validRunID, "second"} {
		if err := writeJSON(&written, map[string]string{"run": id}); err != nil {
			t.Fatalf("write = %v", err)
		}
	}
	lines := strings.Split(strings.TrimSuffix(written.String(), "\n"), "\n")
	if len(lines) != 2 {
		t.Fatalf("output holds %d lines, want one per value: %q", len(lines), written.String())
	}
	for _, line := range lines {
		if !strings.HasPrefix(line, "{") || !strings.HasSuffix(line, "}") {
			t.Fatalf("line %q is not a JSON object on its own", line)
		}
	}
}

// validRunID is a run identifier of the shape store.ValidID admits, so a case
// that means to be refused for its COUNT is not refused for its shape instead.
const validRunID = "01996f90-3415-7cfe-8ff1-600058131b10"

// refusingWriter is declared by task_catalog_cli_test.go; the same writer
// serves here, so the two CLI surfaces are held to one idea of a stream that
// has gone away.
