// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// noServerNamed empties the four variables every run subcommand needs before
// it can reach anyone, so a command that gets past its own argument check
// stops at the environment instead of opening a connection from a test.
func noServerNamed(t *testing.T) {
	t.Helper()
	for _, name := range []string{envServerURL, envServerCA, envClientCert, envClientKey} {
		t.Setenv(name, "")
	}
}

// closedOutput fails every write, which is the only way writeJSON's own
// refusal is reached: the encoder itself never refuses a value the CLI holds.
type closedOutput struct{ err error }

func (w closedOutput) Write([]byte) (int, error) { return 0, w.err }

// soundRunID is an identifier the CLI's own validator accepts, so a table can
// spoil the shape deliberately and know the refusal is about the shape.
func soundRunID(t *testing.T) string {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	if !store.ValidID(id) {
		t.Fatalf("minted identifier %q is not valid", id)
	}
	return id
}

func TestEveryRunSubcommandRefusesItsArgumentsBeforeReachingForAServer(t *testing.T) {
	noServerNamed(t)
	valid := soundRunID(t)
	cases := map[string]struct {
		run   func(context.Context, []string) error
		args  []string
		usage string
	}{
		"status without a run": {showRun, nil, "usage: ra8ci run status RUN_ID"},
		"status with two runs": {showRun, []string{valid, valid}, "usage: ra8ci run status RUN_ID"},
		"cancel without a run": {cancelRun, nil, "usage: ra8ci run cancel RUN_ID"},
		"cancel with two runs": {cancelRun, []string{valid, valid}, "usage: ra8ci run cancel RUN_ID"},
		"cancel of a name that is not a run identifier": {cancelRun, []string{"run-1"},
			"usage: ra8ci run cancel RUN_ID"},
		"logs without an attempt": {showRunLogs, []string{valid},
			"usage: ra8ci run logs [--after SEQ] [--limit 1..8] RUN_ID ATTEMPT_ID"},
		"logs with a flag that is not one": {showRunLogs, []string{"--since", "1", valid, valid},
			"usage: ra8ci run logs [--after SEQ] [--limit 1..8] RUN_ID ATTEMPT_ID"},
		"events without a run": {showRunEvents, nil,
			"usage: ra8ci run events [--after SEQ] [--limit 1..50] RUN_ID"},
		"events with two runs": {showRunEvents, []string{valid, valid},
			"usage: ra8ci run events [--after SEQ] [--limit 1..50] RUN_ID"},
		"events for a name that is not a run identifier": {showRunEvents, []string{"run-1"},
			"usage: ra8ci run events [--after SEQ] [--limit 1..50] RUN_ID"},
		"events with a flag that is not one": {showRunEvents, []string{"--tail", valid},
			"usage: ra8ci run events [--after SEQ] [--limit 1..50] RUN_ID"},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			err := testCase.run(context.Background(), testCase.args)
			if err == nil || err.Error() != testCase.usage {
				t.Fatalf("err=%v; want exactly %q", err, testCase.usage)
			}
		})
	}
}

func TestARunSubcommandWithGoodArgumentsStopsAtTheEnvironment(t *testing.T) {
	noServerNamed(t)
	valid := soundRunID(t)
	cases := map[string]func() error{
		"status": func() error { return showRun(context.Background(), []string{valid}) },
		"cancel": func() error { return cancelRun(context.Background(), []string{valid}) },
		"logs":   func() error { return showRunLogs(context.Background(), []string{valid, valid}) },
		"events": func() error { return showRunEvents(context.Background(), []string{valid}) },
	}
	for name, run := range cases {
		t.Run(name, func(t *testing.T) {
			err := run()
			if err == nil {
				t.Fatal("a run subcommand reached out with no server named")
			}
			// The environment refusal, not the usage line: the arguments were
			// good, so the command got as far as looking for its credentials
			// and named every variable it still needs.
			for _, name := range []string{"ra8ci run", envServerURL, envServerCA, envClientCert, envClientKey} {
				if !strings.Contains(err.Error(), name) {
					t.Fatalf("err=%v; want it to name %s", err, name)
				}
			}
		})
	}
}

func TestSubmitRefusesBeforeItLooksForACheckout(t *testing.T) {
	noServerNamed(t)
	cases := map[string]struct {
		args  []string
		wants []string
	}{
		"nothing at all":    {nil, []string{submitUsage}},
		"tasks but no key":  {[]string{"build"}, []string{submitUsage}},
		"an empty key":      {[]string{"--idempotency-key", "", "build"}, []string{submitUsage}},
		"a key but no task": {[]string{"--idempotency-key", "key-1"}, []string{submitUsage}},
		"a flag that is not one": {[]string{"--retry", "3", "build"},
			[]string{submitUsage, "flag provided but not defined"}},
		"an argument naming no task": {[]string{"--idempotency-key", "key-1", "NAME=VALUE"},
			[]string{submitUsage, "names no task", "arguments follow the task they belong to"}},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			err := submitRun(context.Background(), testCase.args)
			if err == nil {
				t.Fatal("submit was accepted with nothing to submit")
			}
			for _, want := range testCase.wants {
				if !strings.Contains(err.Error(), want) {
					t.Fatalf("err=%v; want it to carry %q", err, want)
				}
			}
			// Every refusal here happens before a checkout is looked for, so
			// none of them can be about the tree the caller happens to be in.
			for _, notWanted := range []string{"repository checkout", "source snapshot", envServerURL} {
				if strings.Contains(err.Error(), notWanted) {
					t.Fatalf("err=%v; want the refusal taken before %q is reached", err, notWanted)
				}
			}
		})
	}
}

func TestSubmitUsageStatesTheGrammarItEnforces(t *testing.T) {
	// The usage line is the only statement of the submit grammar, so it has to
	// name the key it requires and show that arguments follow their task.
	for _, part := range []string{"ra8ci run submit", "--idempotency-key KEY", "TASK [NAME=VALUE...]"} {
		if !strings.Contains(submitUsage, part) {
			t.Fatalf("submit usage %q does not state %q", submitUsage, part)
		}
	}
}

func TestWriteJSONWritesOneDocumentPerLine(t *testing.T) {
	var out bytes.Buffer
	type receipt struct {
		RunID string `json:"run_id"`
		Tasks int    `json:"tasks"`
	}
	if err := writeJSON(&out, receipt{RunID: "run-9", Tasks: 2}); err != nil {
		t.Fatal(err)
	}
	if err := writeJSON(&out, receipt{RunID: "run-10", Tasks: 1}); err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSuffix(out.String(), "\n"), "\n")
	if len(lines) != 2 {
		t.Fatalf("lines=%d (%q); want one document per line so a reader can stream them", len(lines), out.String())
	}
	var first receipt
	if err := json.Unmarshal([]byte(lines[0]), &first); err != nil {
		t.Fatal(err)
	}
	if first.RunID != "run-9" || first.Tasks != 2 {
		t.Fatalf("first=%+v; want the value written whole", first)
	}
	if !strings.HasSuffix(out.String(), "\n") {
		t.Fatal("the last document has no newline; a shell reading line by line would hang on it")
	}
}

func TestWriteJSONReportsAWriterThatRefused(t *testing.T) {
	refusal := errors.New("output pipe closed")
	err := writeJSON(closedOutput{err: refusal}, map[string]string{"run_id": "run-9"})
	if err == nil || !strings.Contains(err.Error(), "write JSON output") {
		t.Fatalf("err=%v; want the write reported as a JSON output failure", err)
	}
	if !errors.Is(err, refusal) {
		t.Fatalf("err=%v; want the writer's own refusal carried inside it", err)
	}
}
