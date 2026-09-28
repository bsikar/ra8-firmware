// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package committerms

import (
	"bytes"
	"context"
	"io"
	"strings"
	"testing"
)

// The gate refuses an incomplete invocation instead of crashing on the very
// line meant to announce the refusal. The writers must be nil interface
// values: a typed nil buffer is a non-nil interface and slides past the
// guard, so such a test would pass while proving nothing.
func TestRunWithoutTheWritersItWasPromised(t *testing.T) {
	message := strings.NewReader("fix(spi): rework the MOSI pin mux\n")
	t.Run("no context", func(t *testing.T) {
		var stdout, stderr bytes.Buffer
		if code := Run(nil, nil, message, &stdout, &stderr); code != 2 {
			t.Fatalf("exit %d, want 2", code)
		}
		if stdout.Len() != 0 {
			t.Fatalf("stdout = %q, want nothing", stdout.String())
		}
		if !strings.Contains(stderr.String(), "invalid input") {
			t.Fatalf("stderr = %q, want the invalid-input line", stderr.String())
		}
	})
	t.Run("no message to read", func(t *testing.T) {
		var stdout, stderr bytes.Buffer
		if code := Run(context.Background(), nil, nil, &stdout, &stderr); code != 2 {
			t.Fatalf("exit %d, want 2", code)
		}
		if !strings.Contains(stderr.String(), "invalid input") {
			t.Fatalf("stderr = %q, want the invalid-input line", stderr.String())
		}
	})
	t.Run("no stdout", func(t *testing.T) {
		var stdout io.Writer
		var stderr bytes.Buffer
		if code := Run(context.Background(), nil, message, stdout, &stderr); code != 2 {
			t.Fatalf("exit %d, want 2", code)
		}
		if !strings.Contains(stderr.String(), "invalid input") {
			t.Fatalf("stderr = %q, want the invalid-input line", stderr.String())
		}
	})
	t.Run("no stderr", func(t *testing.T) {
		var stderr io.Writer
		var stdout bytes.Buffer
		if code := Run(context.Background(), nil, message, &stdout, stderr); code != 2 {
			t.Fatalf("exit %d, want 2", code)
		}
		if stdout.Len() != 0 {
			t.Fatalf("stdout = %q, want nothing", stdout.String())
		}
	})
}

// The annotation is "LEGACY-OK:" with a reason. The word on its own, with no
// colon after it, is somebody talking about the annotation rather than
// claiming one, so its own paragraph stays judged. Each case below keeps the
// mention in the SAME paragraph as the term, so the colon is the only thing
// deciding the outcome.
func TestTheWordAloneWithNoColonIsNotAnOptOut(t *testing.T) {
	for _, message := range []string{
		"ci(gates): widen scope\n\nquoting the MOSI pin label\nA LEGACY-OK would be wrong here\n",
		"ci(gates): widen scope\n\nquoting the MOSI pin label\nLEGACY-OK\n",
		"ci(gates): widen scope\n\nquoting the MOSI pin label\nsee LEGACY-OK in the contributing guide\n",
	} {
		if got := FindViolations(message); len(got) == 0 {
			t.Fatalf("message %q was silenced by a colonless mention", message)
		}
	}
	silenced := FindViolations("ci(gates): widen scope\n\nquoting the MOSI pin label\nLEGACY-OK: datasheet pin label quoted verbatim\n")
	if len(silenced) != 0 {
		t.Fatalf("a complete annotation did not silence its own paragraph: %v", silenced)
	}
}
