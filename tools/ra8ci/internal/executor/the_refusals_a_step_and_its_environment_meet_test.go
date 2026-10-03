// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// occupiedPath returns an absolute path whose parent is a regular file, so
// resolving it fails with something other than "does not exist". A missing
// path is walked back to its nearest living parent and accepted; a path that
// runs THROUGH a file cannot be resolved at all, which is the refusal these
// tests are about.
func occupiedPath(t *testing.T) string {
	t.Helper()
	file := filepath.Join(t.TempDir(), "occupied")
	if err := os.WriteFile(file, []byte("not a directory\n"), 0o600); err != nil {
		t.Fatalf("plant file: %v", err)
	}
	return filepath.Join(file, "below")
}

func TestBoundTaskRefusesADefinitionTheCatalogRejects(t *testing.T) {
	writers := func(string) (io.Writer, io.Writer) { return io.Discard, io.Discard }
	result, err := runBoundTask(context.Background(), t.TempDir(), catalog.Task{}, nil, writers, 0)
	if !errors.Is(err, catalog.ErrInvalidCatalog) {
		t.Fatalf("error = %v", err)
	}
	if result.ExitCode != -1 || len(result.Steps) != 0 {
		t.Fatalf("a refused definition reported a run: %+v", result)
	}
}

func TestCleanEnvironmentRefusesAScratchDirectoryItCannotResolve(t *testing.T) {
	root := t.TempDir()
	scratch := occupiedPath(t)
	t.Setenv("TMPDIR", scratch)
	env, err := cleanEnvironment(root)
	if !errors.Is(err, ErrUnsafeEnvironment) || env != nil {
		t.Fatalf("env = %v, error = %v", env, err)
	}
}

func TestIsWithinRefusesARootAndCandidateItCannotRelate(t *testing.T) {
	// A relative root and an absolute candidate both resolve, and then no
	// path from one to the other exists, so the answer is an error rather
	// than a false "outside the checkout".
	within, err := isWithin(".", t.TempDir())
	if within || !errors.Is(err, ErrUnsafeEnvironment) {
		t.Fatalf("within = %v, error = %v", within, err)
	}
}

func TestResolveTaskProgramRefusesARootThatDoesNotResolve(t *testing.T) {
	absent := filepath.Join(t.TempDir(), "gone")
	program, err := resolveTaskProgram(absent, "tools/ra8ci/thing")
	if program != "" || !errors.Is(err, ErrUnreviewedTask) {
		t.Fatalf("program = %q, error = %v", program, err)
	}
}

// expiringWhenWritten expires the moment the step writes its first byte, which
// places the verdict INSIDE the gate rather than in front of it. A context
// that is already spent is refused at the front door instead, so a fixed
// deadline cannot reach the branch these tests are about.
type expiringWhenWritten struct {
	context.Context
	cause error
	spent *bool
}

func (ctx expiringWhenWritten) Err() error {
	if *ctx.spent {
		return ctx.cause
	}
	return nil
}

type writerThatSpendsTheContext struct{ spent *bool }

func (writer writerThatSpendsTheContext) Write(data []byte) (int, error) {
	*writer.spent = true
	return len(data), nil
}

func TestGateStepThatExpiresWhileItRuns(t *testing.T) {
	for _, testCase := range []struct {
		name     string
		cause    error
		timedOut bool
	}{
		{name: "deadline", cause: context.DeadlineExceeded, timedOut: true},
		{name: "cancel", cause: context.Canceled},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			spent := false
			sink := writerThatSpendsTheContext{spent: &spent}
			step := catalog.Step{Name: "gate", Program: "ra8ci:no-null"}
			ctx := expiringWhenWritten{Context: context.Background(), cause: testCase.cause, spent: &spent}
			result, err := runStep(ctx, t.TempDir(), nil, step, sink, sink, 0)
			if err != nil {
				t.Fatalf("error = %v", err)
			}
			if !spent {
				t.Fatal("the gate wrote nothing, so the context never expired mid-run")
			}
			if result.TimedOut != testCase.timedOut || result.Cancelled == testCase.timedOut {
				t.Fatalf("timed out = %v, cancelled = %v", result.TimedOut, result.Cancelled)
			}
			if result.Name != "gate" {
				t.Fatalf("step name = %q", result.Name)
			}
		})
	}
}

func TestGateStepPropagatesALogSinkFailure(t *testing.T) {
	step := catalog.Step{Name: "gate", Program: "ra8ci:no-null"}
	result, err := runStep(context.Background(), t.TempDir(), nil, step, failingWriter{}, failingWriter{}, 0)
	if err == nil {
		t.Fatal("a broken log sink was accepted")
	}
	if result.StdoutBytes != 0 || result.StderrBytes != 0 {
		t.Fatalf("bytes counted through a failed sink: %+v", result)
	}
}
