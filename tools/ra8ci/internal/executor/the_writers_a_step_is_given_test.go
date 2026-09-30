// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"testing"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// plantedCheckout builds the smallest tree VerifyCheckout will accept: a .git
// marker, the embedded manifest and its digest where the checkout carries
// them, and a stand-in for the script the reviewed task dispatches. The task
// definition itself still comes from the catalog, so nothing here can admit a
// task the executor would not.
func plantedCheckout(t *testing.T, script string) string {
	t.Helper()
	root := t.TempDir()
	if err := os.Mkdir(filepath.Join(root, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	manifestDir := filepath.Join(root, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(manifestDir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(manifestDir, "tasks.json"), embedded.Manifest(), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(manifestDir, "sha256.txt"), embedded.Digest(), 0600); err != nil {
		t.Fatal(err)
	}
	scriptDir := filepath.Join(root, "scripts", "checks")
	if err := os.MkdirAll(scriptDir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(scriptDir, "format_tree.sh"), []byte(script), 0700); err != nil {
		t.Fatal(err)
	}
	return root
}

func formatCheck(t *testing.T) catalog.Task {
	t.Helper()
	definitions, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	task, found := definitions.Task("format-check")
	if !found {
		t.Skip("the catalog no longer declares format-check")
	}
	return task
}

// A caller who supplies a step-writer selector gets the step's output where
// the selector says, and the writers passed beside it are not used at all.
// The selector is asked by step name, which is what lets a caller keep one
// step's output apart from another's.
func TestASuppliedStepWriterSelectorDecidesWhereAStepIsWritten(t *testing.T) {
	root := plantedCheckout(t, "#!/bin/sh\nprintf 'chosen-out\\n'\nprintf 'chosen-err\\n' >&2\n")
	var chosenOut, chosenErr, fallbackOut, fallbackErr bytes.Buffer
	var asked []string

	result, err := Run(context.Background(), root, formatCheck(t), &fallbackOut, &fallbackErr,
		func(step string) (io.Writer, io.Writer) {
			asked = append(asked, step)
			return &chosenOut, &chosenErr
		})
	if err != nil {
		t.Fatalf("run = %v", err)
	}
	if result.ExitCode != 0 {
		t.Fatalf("result = %+v, want a clean run", result)
	}
	if len(asked) != 1 || asked[0] != "format-tree-check" {
		t.Fatalf("selector was asked for %v, want the reviewed step name once", asked)
	}
	if chosenOut.String() != "chosen-out\n" || chosenErr.String() != "chosen-err\n" {
		t.Fatalf("selector writers got out=%q err=%q", chosenOut.String(), chosenErr.String())
	}
	if fallbackOut.Len() != 0 || fallbackErr.Len() != 0 {
		t.Fatalf("the writers beside the selector were still used: out=%q err=%q",
			fallbackOut.String(), fallbackErr.String())
	}
}

// Without a selector the same run writes to the writers it was handed, so the
// pinning above is about the selector and not about this task being quiet.
func TestWithoutASelectorTheStepWritesToTheWritersItWasHanded(t *testing.T) {
	root := plantedCheckout(t, "#!/bin/sh\nprintf 'plain-out\\n'\n")
	var stdout, stderr bytes.Buffer

	result, err := Run(context.Background(), root, formatCheck(t), &stdout, &stderr)
	if err != nil || result.ExitCode != 0 {
		t.Fatalf("result = %+v, error = %v", result, err)
	}
	if stdout.String() != "plain-out\n" {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

// And the two refusals around it: more than one selector is a caller mistake
// rather than a merge, and a nil selector is refused rather than silently
// swallowing the step's output.
func TestTheStepWriterSelectorIsSingularAndNeverNil(t *testing.T) {
	root := plantedCheckout(t, "#!/bin/sh\nprintf 'unreached\\n'\n")
	both := func(string) (io.Writer, io.Writer) { return io.Discard, io.Discard }

	if _, err := Run(context.Background(), root, formatCheck(t), io.Discard, io.Discard, both, both); err == nil {
		t.Fatal("two step-writer selectors were accepted")
	}
	if _, err := Run(context.Background(), root, formatCheck(t), io.Discard, io.Discard, nil); err == nil {
		t.Fatal("a nil step-writer selector was accepted")
	}
}
