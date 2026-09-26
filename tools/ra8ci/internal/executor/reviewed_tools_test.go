// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"context"
	"errors"
	"io"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The catalog refuses a reviewed step naming an ra8ci: tool nothing
// implements, and it can only do that honestly if its registry is the set this
// package dispatches in process. The catalog cannot import the executor (the
// dependency runs the other way), so the registry lives there and the
// agreement is pinned here.
//
// A name that reached resolveTaskProgram would come back as ErrToolMissing,
// the "this runner is missing a tool" error, which is exactly the answer the
// registry exists to keep out of a reviewed manifest. So the assertion is that
// no reviewed tool produces it.
func TestEveryReviewedToolProgramIsDispatchedInProcess(t *testing.T) {
	programs := catalog.ReviewedToolPrograms()
	if len(programs) == 0 {
		t.Fatal("the catalog names no reviewed tool at all, so this test proves nothing")
	}
	root := t.TempDir()
	for _, program := range programs {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		step := catalog.Step{Name: "reviewed-tool", Program: program, Args: []string{"--selftest"}}
		_, err := runStep(ctx, root, nil, step, io.Discard, io.Discard, time.Second)
		cancel()
		if errors.Is(err, ErrToolMissing) {
			t.Fatalf("reviewed tool %q was looked up as a program rather than dispatched in process", program)
		}
	}
}

// The other direction: a tool name the catalog does not list is exactly the
// one this package cannot dispatch, so it reaches the program lookup and fails
// as a missing tool. That is the failure the registry replaces with a refusal
// at review time, and it is worth pinning that it is still what happens to a
// name that gets past review some other way.
func TestAnUnlistedToolProgramFallsThroughToTheProgramLookup(t *testing.T) {
	if catalog.IsReviewedToolProgram("ra8ci:not-a-tool") {
		t.Fatal("ra8ci:not-a-tool is listed as reviewed; pick another name for this test")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	step := catalog.Step{Name: "unlisted-tool", Program: "ra8ci:not-a-tool"}
	result, err := runStep(ctx, t.TempDir(), nil, step, io.Discard, io.Discard, time.Second)
	if !errors.Is(err, ErrToolMissing) {
		t.Fatalf("runStep error = %v, want ErrToolMissing", err)
	}
	if result.ExitCode != -1 {
		t.Fatalf("unlisted tool exit code = %d, want -1", result.ExitCode)
	}
}
