// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	"bytes"
	"context"
	"io"
	"strings"
	"testing"
)

// TestRunWithoutTheWritersItWasPromised pins the four incomplete invocations of
// Run. Each answers 2 and writes nothing to stdout. The interesting one is the
// last: stderr itself is missing, so the refusal has nowhere to be announced,
// and announcing it anyway is what turned a caller's mistake into a crash.
func TestRunWithoutTheWritersItWasPromised(t *testing.T) {
	t.Parallel()

	root := t.TempDir()

	for _, tc := range []struct {
		name      string
		ctx       context.Context
		root      string
		withOut   bool
		withErr   bool
		announced bool
	}{
		{name: "no context", root: root, withOut: true, withErr: true, announced: true},
		{name: "no root", ctx: context.Background(), withOut: true, withErr: true, announced: true},
		{name: "no stdout", ctx: context.Background(), root: root, withErr: true, announced: true},
		{name: "no stderr", ctx: context.Background(), root: root, withOut: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var out, errOut bytes.Buffer

			var stdout io.Writer
			if tc.withOut {
				stdout = &out
			}
			var stderr io.Writer
			if tc.withErr {
				stderr = &errOut
			}

			if code := Run(tc.ctx, tc.root, nil, stdout, stderr); code != 2 {
				t.Fatalf("exit %d, want 2", code)
			}
			if out.Len() != 0 {
				t.Fatalf("stdout carried %q, want nothing", out.String())
			}
			if tc.announced != strings.Contains(errOut.String(), "invalid input") {
				t.Fatalf("stderr carried %q, announced refusal expected: %t", errOut.String(), tc.announced)
			}
		})
	}
}
