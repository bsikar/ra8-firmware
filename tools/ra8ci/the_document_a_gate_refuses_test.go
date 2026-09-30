// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// `gate` reads one branch name and reports the checks that branch requires.
// It is the last of the github commands whose document was held only to being
// refused: the existing test asserts that five bad documents are turned away
// and write nothing, but never what any of them was refused FOR, and the size
// bound was not pinned at all.
//
// Wording matters here for the same reason it does across publish and
// reconcile: an operator who mistypes a branch document and an operator whose
// document is simply too large need to be told apart, and both need telling
// apart from a deployment that cannot reach GitHub. gateEnv leaves a private
// key file that does not exist, so a refusal naming the key means the document
// was accepted and a refusal naming the document means the key was never
// opened.

// gated runs one gate and reports what it wrote alongside its refusal. The
// document this command writes is the next command's input, so a refusal that
// wrote half of one would be read by required-checks as a gate.
func gated(t *testing.T, document string) (string, error) {
	t.Helper()
	var out strings.Builder
	err := githubGate(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

// gateDocumentOfBytes pads a readable branch document with insignificant
// whitespace to exactly n bytes. The decoder runs under a limit reader of the
// bound plus one, so only a whole document one byte over reaches the size
// refusal; a document far over it is truncated and refused as unreadable.
func gateDocumentOfBytes(t *testing.T, n int) string {
	t.Helper()
	const opening = `{"branch":"main"`
	const closing = `}`
	padding := n - len(opening) - len(closing)
	if padding < 0 {
		t.Fatalf("a %d byte document cannot hold a branch name", n)
	}
	document := opening + strings.Repeat(" ", padding) + closing
	if len(document) != n {
		t.Fatalf("document is %d bytes, want %d", len(document), n)
	}
	return document
}

func TestTheGateRefusesADocumentInItsOwnWords(t *testing.T) {
	refusals := map[string]struct{ document, want string }{
		"nothing named":                      {`{}`, "no branch named"},
		"a branch named as nothing":          {`{"branch":""}`, "no branch named"},
		"a list where an object belongs":     {`["main"]`, "read the branch to report"},
		"a field the document does not have": {`{"ref":"main"}`, "read the branch to report"},
		"a second document after the first": {`{"branch":"main"}{"branch":"dev"}`,
			"read the branch to report: trailing content after the document"},
	}
	for name, testCase := range refusals {
		t.Run(name, func(t *testing.T) {
			gateEnv(t)
			wrote, err := gated(t, testCase.document)
			if err == nil {
				t.Fatalf("a bad document was gated: %q", wrote)
			}
			if !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("refusal=%v; want %q named", err, testCase.want)
			}
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal=%v; the key was opened over a bad document", err)
			}
			if wrote != "" {
				t.Fatalf("a refused gate wrote %q", wrote)
			}
		})
	}
}

// The bound, exact on both sides. A branch name is short, so the whole point
// of this bound is that whatever else is piped in is refused by its size
// rather than read: at the bound the document is taken whole and the command
// goes on to open the key, one byte over it never is.
func TestTheGateDocumentBoundIsExact(t *testing.T) {
	t.Run("at the bound", func(t *testing.T) {
		gateEnv(t)
		wrote, err := gated(t, gateDocumentOfBytes(t, maxGateRequestBytes))
		if err == nil {
			t.Fatalf("a gate answered with no key on disk: %q", wrote)
		}
		if strings.Contains(err.Error(), "read the branch to report") {
			t.Fatalf("refusal=%v; a document at the bound was read as a bad document", err)
		}
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; want the key named", err)
		}
	})
	t.Run("one byte over", func(t *testing.T) {
		gateEnv(t)
		wrote, err := gated(t, gateDocumentOfBytes(t, maxGateRequestBytes+1))
		if err == nil {
			t.Fatalf("an oversize document was gated: %q", wrote)
		}
		for _, want := range []string{"read the branch to report", "larger than"} {
			if !strings.Contains(err.Error(), want) {
				t.Fatalf("refusal=%v; want %q named", err, want)
			}
		}
		if wrote != "" {
			t.Fatalf("a refused gate wrote %q", wrote)
		}
	})
}
