// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// `reconcile` surveys what is already on a commit and says what is missing.
// It reads the same document `publish` does, through the same decoder and the
// same planner, and it must refuse a bad one in the same words: an operator
// reading two different phrases for one malformed document learns nothing
// about which command they actually mistyped.
//
// The publish side of that pairing is pinned in the_plans_a_publish_refuses
// test; reconcile's own decoder, planner arms and bound were not. The absent
// private key from publishCheckRunEnv is the ordering marker throughout: a
// refusal naming the key means the document was accepted, and a refusal
// naming the document means the key was never opened.

// reconciled runs one reconcile and reports what it wrote alongside its
// refusal. A refused survey that wrote part of a report is worse than the
// refusal, since those lines are an operator's record of the commit's state.
func reconciled(t *testing.T, document string) (string, error) {
	t.Helper()
	var out strings.Builder
	err := githubReconcileCheckRuns(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

func TestAReconcileRefusesADocumentInThePublishWords(t *testing.T) {
	task := publishCheckRunEnv(t)
	refusals := map[string]struct{ document, want string }{
		"a field the document does not have": {
			`{"head_sha":"` + shadowCompareHead + `","runs":[],"commits":[]}`,
			"read task outcomes",
		},
		"a second document after the first": {
			publishAsk(task) + ` {"head_sha":"` + shadowCompareHead + `","runs":[]}`,
			"read task outcomes: trailing content after the document",
		},
		"nothing to survey": {
			`{"head_sha":"` + shadowCompareHead + `","runs":[]}`,
			"no task outcomes to publish",
		},
		"one task twice on one commit": {
			`{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"` + task +
				`","state":"succeeded"},{"task":"` + task + `","state":"failed"}]}`,
			`appears twice for this commit`,
		},
		"a task the correspondence does not cover": {
			`{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"nowhere","state":"failed"}]}`,
			"not covered by the declared correspondence",
		},
	}
	for name, testCase := range refusals {
		t.Run(name, func(t *testing.T) {
			publishCheckRunEnv(t)
			wrote, err := reconciled(t, testCase.document)
			if err == nil {
				t.Fatalf("a bad document was surveyed: %q", wrote)
			}
			if !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("refusal=%v; want %q named", err, testCase.want)
			}
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal=%v; the key was opened over a bad document", err)
			}
			if wrote != "" {
				t.Fatalf("a refused survey wrote %q", wrote)
			}
		})
	}
}

// The counterpart, which is what stops every refusal above reading as
// reconcile refusing whatever it is handed: a document publish would accept is
// planned whole and the command goes on to open the key.
func TestAReconcilableDocumentReachesTheAppKey(t *testing.T) {
	task := publishCheckRunEnv(t)
	wrote, err := reconciled(t, publishAsk(task))
	if err == nil {
		t.Fatalf("a survey answered with no key on disk: %q", wrote)
	}
	if !strings.Contains(err.Error(), "GitHub App private key") {
		t.Fatalf("refusal=%v; want the key named", err)
	}
	if wrote != "" {
		t.Fatalf("a refused survey wrote %q", wrote)
	}
}

// The bound is the publish bound, read through reconcile's own decoder, and it
// is exact on both sides. A document one byte over is refused by its size
// rather than by whatever the truncated remainder happened to parse as.
func TestTheReconcileDocumentBoundIsExact(t *testing.T) {
	t.Run("at the bound", func(t *testing.T) {
		task := publishCheckRunEnv(t)
		wrote, err := reconciled(t, publishDocumentOfBytes(t, task, maxCheckRunPublishBytes))
		if err == nil {
			t.Fatalf("a survey answered with no key on disk: %q", wrote)
		}
		// Reaching the key is the pin: a document exactly at the bound was
		// read whole, planned, and accepted.
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; a document at the bound was not accepted", err)
		}
	})
	t.Run("one byte over", func(t *testing.T) {
		task := publishCheckRunEnv(t)
		wrote, err := reconciled(t, publishDocumentOfBytes(t, task, maxCheckRunPublishBytes+1))
		if err == nil {
			t.Fatalf("an oversize document was surveyed: %q", wrote)
		}
		for _, want := range []string{"read task outcomes", "larger than"} {
			if !strings.Contains(err.Error(), want) {
				t.Fatalf("refusal=%v; want %q named", err, want)
			}
		}
		if wrote != "" {
			t.Fatalf("a refused survey wrote %q", wrote)
		}
	})
}
