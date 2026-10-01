// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

import (
	"bytes"
	"context"
	"io"
	"strings"
	"testing"
)

// The gate is handed its context, its root and the two writers it reports
// through. Missing any of them it exits 2 without a crash, and it says so on
// stderr when a stderr is what it still has.
func TestRunWithoutTheWritersItWasPromised(t *testing.T) {
	t.Run("no context", func(t *testing.T) {
		var absent context.Context
		var stdout, stderr bytes.Buffer
		if code := Run(absent, t.TempDir(), nil, &stdout, &stderr); code != 2 {
			t.Fatalf("a run with no context answered %d", code)
		}
		if stdout.Len() != 0 {
			t.Fatalf("stdout carried %q", stdout.String())
		}
		if !strings.Contains(stderr.String(), "invalid input") {
			t.Fatalf("stderr did not name the refusal: %q", stderr.String())
		}
	})

	t.Run("no root", func(t *testing.T) {
		var stdout, stderr bytes.Buffer
		if code := Run(context.Background(), "", nil, &stdout, &stderr); code != 2 {
			t.Fatalf("a run with no root answered %d", code)
		}
		if stdout.Len() != 0 {
			t.Fatalf("stdout carried %q", stdout.String())
		}
		if !strings.Contains(stderr.String(), "invalid input") {
			t.Fatalf("stderr did not name the refusal: %q", stderr.String())
		}
	})

	// The writers must be nil io.Writer interface values. A typed nil buffer is
	// a non-nil interface and would slide straight past the guard, leaving the
	// test passing while proving nothing.
	t.Run("no stdout", func(t *testing.T) {
		var absent io.Writer
		var stderr bytes.Buffer
		if code := Run(context.Background(), t.TempDir(), nil, absent, &stderr); code != 2 {
			t.Fatalf("a run with no stdout answered %d", code)
		}
		if !strings.Contains(stderr.String(), "invalid input") {
			t.Fatalf("stderr did not name the refusal: %q", stderr.String())
		}
	})

	t.Run("no stderr", func(t *testing.T) {
		var absent io.Writer
		var stdout bytes.Buffer
		if code := Run(context.Background(), t.TempDir(), nil, &stdout, absent); code != 2 {
			t.Fatalf("a run with no stderr answered %d", code)
		}
		if stdout.Len() != 0 {
			t.Fatalf("stdout carried %q", stdout.String())
		}
	})
}
