// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// `pull-request-evidence` reads a document naming pull requests, then reads
// each one's head from GitHub and the workflow runs on it. Everything the
// document itself can be wrong about is judged before any of that happens, so
// a bad ask is reported as a bad ask rather than as a failure to reach
// GitHub. These pin that ordering and the bound around it.
//
// publishCheckRunEnv configures a complete publishing environment whose
// private key file does not exist, so an ask that is accepted refuses at the
// key instead. That absent key is the marker: a refusal naming the key means
// the ask got through, and a refusal naming the ask means the key was never
// opened.

// evidenceAsk is an ordinary one-pull-request ask, the shape every case here
// varies from.
const evidenceAsk = `{"workflow":"Checks","threshold":1,"pull_requests":` +
	`[{"number":1589,"plane":[{"task":"format-check","head_sha":"` + shadowCompareHead +
	`","observed":"success"}]}]}`

// evidenceDocumentOfBytes builds a valid ask padded with insignificant
// whitespace to exactly n bytes, which is how the size bound is approached
// from both sides without a document the decoder would refuse for some other
// reason.
func evidenceDocumentOfBytes(t *testing.T, n int) string {
	t.Helper()
	opening := strings.TrimSuffix(evidenceAsk, "}")
	const closing = `}`
	padding := n - len(opening) - len(closing)
	if padding < 0 {
		t.Fatalf("a %d byte document cannot hold the ask", n)
	}
	document := opening + strings.Repeat(" ", padding) + closing
	if len(document) != n {
		t.Fatalf("document is %d bytes, want %d", len(document), n)
	}
	return document
}

// gathered runs one gather and reports what it wrote alongside its refusal,
// since a refusal that wrote partial evidence is a worse failure than the
// refusal itself.
func gathered(t *testing.T, document string) (string, error) {
	t.Helper()
	var out strings.Builder
	err := githubPullRequestEvidence(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

func TestGatheringEvidenceJudgesTheAskBeforeItOpensTheAppKey(t *testing.T) {
	asks := map[string]struct{ document, want string }{
		"no workflow named": {`{"threshold":1,"pull_requests":` +
			`[{"number":1589,"plane":[{"task":"format-check","head_sha":"` + shadowCompareHead +
			`","observed":"success"}]}]}`, "no workflow named"},
		"no readiness threshold": {`{"workflow":"Checks","pull_requests":` +
			`[{"number":1589,"plane":[{"task":"format-check","head_sha":"` + shadowCompareHead +
			`","observed":"success"}]}]}`, "no readiness threshold stated"},
		"nothing to gather": {`{"workflow":"Checks","threshold":1,"pull_requests":[]}`,
			"no pull requests to gather"},
		"a pull request with no number": {`{"workflow":"Checks","threshold":1,"pull_requests":` +
			`[{"number":0,"plane":[{"task":"format-check","head_sha":"` + shadowCompareHead +
			`","observed":"success"}]}]}`, "a pull request with no number"},
		"one pull request asked for twice": {`{"workflow":"Checks","threshold":1,"pull_requests":` +
			`[{"number":1589,"plane":[{"task":"format-check","head_sha":"` + shadowCompareHead +
			`","observed":"success"}]},{"number":1589,"plane":[{"task":"format-check","head_sha":"` +
			shadowCompareHead + `","observed":"success"}]}]}`, "1589 named twice"},
		"a pull request with nothing to compare": {`{"workflow":"Checks","threshold":1,` +
			`"pull_requests":[{"number":1589,"plane":[]}]}`,
			"1589 has no plane outcomes to compare against"},
		"a field the document does not have": {`{"workflow":"Checks","threshold":1,"repository":"ra8",` +
			`"pull_requests":[{"number":1589,"plane":[{"task":"format-check","head_sha":"` +
			shadowCompareHead + `","observed":"success"}]}]}`, "read the pull requests to gather"},
		"a second document after the first": {evidenceAsk + `{"workflow":"Checks"}`,
			"trailing content after the document"},
	}
	for name, testCase := range asks {
		t.Run(name, func(t *testing.T) {
			publishCheckRunEnv(t)
			wrote, err := gathered(t, testCase.document)
			if err == nil {
				t.Fatalf("a bad ask answered: %q", wrote)
			}
			if !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("refusal=%v; want %q named", err, testCase.want)
			}
			// The key is the ordering marker. A refusal that reached it
			// means the ask was accepted, which is the thing being denied.
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal=%v; the key was opened over a bad ask", err)
			}
			if wrote != "" {
				t.Fatalf("a refused gather wrote %q", wrote)
			}
		})
	}
}

// The counterpart: a sound ask is accepted and the command goes on to open the
// key. Without it, every refusal above reads as the command refusing whatever
// it is handed.
func TestGatheringEvidenceTakesASoundAskAsFarAsTheAppKey(t *testing.T) {
	publishCheckRunEnv(t)
	wrote, err := gathered(t, evidenceAsk)
	if err == nil {
		t.Fatalf("a gather answered with no key on disk: %q", wrote)
	}
	if !strings.Contains(err.Error(), "GitHub App private key") {
		t.Fatalf("refusal=%v; want the key named", err)
	}
	if wrote != "" {
		t.Fatalf("a refused gather wrote %q", wrote)
	}
}

// The document bound is exact on both sides, and a document over it is refused
// by its own size rather than by whatever the truncated remainder happened to
// parse as.
func TestTheEvidenceDocumentBoundIsExact(t *testing.T) {
	t.Run("at the bound", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := gathered(t, evidenceDocumentOfBytes(t, maxPullRequestEvidenceBytes))
		if err == nil {
			t.Fatalf("a gather answered with no key on disk: %q", wrote)
		}
		// Reaching the key is the pin: a document exactly at the bound was
		// read whole and accepted.
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; a document at the bound was not accepted", err)
		}
	})
	t.Run("one byte over", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := gathered(t, evidenceDocumentOfBytes(t, maxPullRequestEvidenceBytes+1))
		if err == nil {
			t.Fatalf("an oversize gather answered: %q", wrote)
		}
		for _, want := range []string{"pull requests to gather", "larger than"} {
			if !strings.Contains(err.Error(), want) {
				t.Fatalf("refusal=%v; want %q named", err, want)
			}
		}
		if wrote != "" {
			t.Fatalf("a refused gather wrote %q", wrote)
		}
	})
}
