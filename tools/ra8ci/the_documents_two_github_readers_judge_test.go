// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"strings"
	"testing"
)

// `actions-run` collects one workflow run, `pull-request` looks one pull
// request up. Both read a document first, and both judge it before a reader
// exists, so a bad document is reported as a bad document rather than as a
// failure to reach GitHub. These pin that ordering and the bounds around it.
//
// publishCheckRunEnv configures a complete publishing environment whose
// private key file does not exist, so a document that is accepted refuses at
// the key instead. That absent key is the marker: a refusal naming the key
// means the document got through, and a refusal naming the document means the
// key was never opened.

// collected runs `actions-run` and reports what it wrote alongside its
// refusal, since a refusal that wrote a partial document is a worse failure
// than the refusal itself.
func collected(t *testing.T, document string) (string, error) {
	t.Helper()
	var out strings.Builder
	err := githubActionsRun(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

// lookedUp runs `pull-request` the same way.
func lookedUp(t *testing.T, document string) (string, error) {
	t.Helper()
	var out strings.Builder
	err := githubPullRequestRuns(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

// actionsRunAsk is an ordinary one-outcome ask, the shape the cases vary from.
const actionsRunAsk = `{"run_id":9912,"plane":[{"task":"format-check","head_sha":"` +
	shadowCompareHead + `","observed":"success"}]}`

// pullRequestAsk is the whole of what `pull-request` reads.
const pullRequestAsk = `{"number":1589}`

// paddedTo returns the document padded with insignificant whitespace to
// exactly n bytes, which is how a size bound is approached from both sides
// without a document the decoder would refuse for some other reason.
func paddedTo(t *testing.T, document string, n int) string {
	t.Helper()
	opening := strings.TrimSuffix(document, "}")
	const closing = `}`
	padding := n - len(opening) - len(closing)
	if padding < 0 {
		t.Fatalf("a %d byte document cannot hold the ask", n)
	}
	padded := opening + strings.Repeat(" ", padding) + closing
	if len(padded) != n {
		t.Fatalf("document is %d bytes, want %d", len(padded), n)
	}
	return padded
}

func TestCollectingARunJudgesTheDocumentBeforeItOpensTheAppKey(t *testing.T) {
	documents := map[string]struct{ document, want string }{
		"no workflow run named": {`{"run_id":0,"plane":[{"task":"format-check","head_sha":"` +
			shadowCompareHead + `","observed":"success"}]}`, "no workflow run named"},
		"a run identified backwards": {`{"run_id":-9912,"plane":[{"task":"format-check","head_sha":"` +
			shadowCompareHead + `","observed":"success"}]}`, "no workflow run named"},
		"nothing to compare against": {`{"run_id":9912,"plane":[]}`,
			"no plane outcomes to compare against"},
		"a field the document does not have": {`{"run_id":9912,"repository":"ra8","plane":` +
			`[{"task":"format-check","head_sha":"` + shadowCompareHead + `","observed":"success"}]}`,
			"read the run to collect"},
		"a second document after the first": {actionsRunAsk + `{"run_id":9913}`,
			"trailing content after the document"},
	}
	for name, testCase := range documents {
		t.Run(name, func(t *testing.T) {
			publishCheckRunEnv(t)
			wrote, err := collected(t, testCase.document)
			if err == nil {
				t.Fatalf("a bad document answered: %q", wrote)
			}
			if !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("refusal=%v; want %q named", err, testCase.want)
			}
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal=%v; the key was opened over a bad document", err)
			}
			if wrote != "" {
				t.Fatalf("a refused collection wrote %q", wrote)
			}
		})
	}
}

func TestLookingUpAPullRequestJudgesTheDocumentBeforeItOpensTheAppKey(t *testing.T) {
	documents := map[string]struct{ document, want string }{
		"no pull request named":             {`{"number":0}`, "no pull request named"},
		"a pull request numbered backwards": {`{"number":-1589}`, "no pull request named"},
		"a field the document does not have": {`{"number":1589,"repository":"ra8"}`,
			"read the pull request to look at"},
		"a second document after the first": {pullRequestAsk + pullRequestAsk,
			"trailing content after the document"},
	}
	for name, testCase := range documents {
		t.Run(name, func(t *testing.T) {
			publishCheckRunEnv(t)
			wrote, err := lookedUp(t, testCase.document)
			if err == nil {
				t.Fatalf("a bad document answered: %q", wrote)
			}
			if !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("refusal=%v; want %q named", err, testCase.want)
			}
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal=%v; the key was opened over a bad document", err)
			}
			if wrote != "" {
				t.Fatalf("a refused lookup wrote %q", wrote)
			}
		})
	}
}

// The counterparts, which are what stop every refusal above reading as these
// commands refusing whatever they are handed: a sound document is accepted and
// the command goes on to open the key.
func TestBothReadersTakeASoundDocumentAsFarAsTheAppKey(t *testing.T) {
	t.Run("a run to collect", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := collected(t, actionsRunAsk)
		if err == nil {
			t.Fatalf("a collection answered with no key on disk: %q", wrote)
		}
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; want the key named", err)
		}
		if wrote != "" {
			t.Fatalf("a refused collection wrote %q", wrote)
		}
	})
	t.Run("a pull request to look at", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := lookedUp(t, pullRequestAsk)
		if err == nil {
			t.Fatalf("a lookup answered with no key on disk: %q", wrote)
		}
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; want the key named", err)
		}
		if wrote != "" {
			t.Fatalf("a refused lookup wrote %q", wrote)
		}
	})
}

// Each bound is exact on both sides, and a document over one is refused by its
// own size rather than by whatever the truncated remainder happened to parse
// as. The two bounds are deliberately different sizes, so this also pins that
// each command carries its own.
func TestBothDocumentBoundsAreExact(t *testing.T) {
	bounds := map[string]struct {
		ask   string
		bound int
		run   func(*testing.T, string) (string, error)
		want  string
	}{
		"a run to collect":          {actionsRunAsk, maxActionsRunRequestBytes, collected, "run to collect"},
		"a pull request to look at": {pullRequestAsk, maxPullRequestRequestBytes, lookedUp, "pull request to look at"},
	}
	for name, testCase := range bounds {
		t.Run(name+" at the bound", func(t *testing.T) {
			publishCheckRunEnv(t)
			wrote, err := testCase.run(t, paddedTo(t, testCase.ask, testCase.bound))
			if err == nil {
				t.Fatalf("answered with no key on disk: %q", wrote)
			}
			// Reaching the key is the pin: a document exactly at the bound
			// was read whole and accepted.
			if !strings.Contains(err.Error(), "GitHub App private key") {
				t.Fatalf("refusal=%v; a document at the bound was not accepted", err)
			}
		})
		t.Run(name+" one byte over", func(t *testing.T) {
			publishCheckRunEnv(t)
			wrote, err := testCase.run(t, paddedTo(t, testCase.ask, testCase.bound+1))
			if err == nil {
				t.Fatalf("an oversize document answered: %q", wrote)
			}
			for _, want := range []string{testCase.want, "larger than"} {
				if !strings.Contains(err.Error(), want) {
					t.Fatalf("refusal=%v; want %q named", err, want)
				}
			}
			if wrote != "" {
				t.Fatalf("a refused document wrote %q", wrote)
			}
		})
	}
}
