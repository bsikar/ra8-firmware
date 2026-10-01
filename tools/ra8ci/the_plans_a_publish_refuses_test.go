// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"context"
	"os"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// `publish-check-runs` is the one command here that WRITES to GitHub, so what
// it refuses before a publisher exists is the whole of its safety: a check run
// cannot be taken back, and a document whose third task is unpublishable must
// be refused with nothing posted rather than with the first two on the commit.
//
// publishCheckRunEnv configures a complete publishing environment whose
// private key file does not exist, so a document that gets as far as building
// a publisher refuses there. That absent key is the ordering marker: a
// refusal naming the key means the document was read and planned first, and a
// refusal naming the document or the plan means nothing was ever built.

// publishAsk is an ordinary one-task document for the named task.
func publishAsk(task string) string {
	return `{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"` + task + `","state":"succeeded"}]}`
}

// publishDocumentOfBytes pads a publishable document with insignificant
// whitespace to exactly n bytes. The decoder runs under a limit reader of the
// bound plus one, so a document far over the bound is truncated and refused
// as unreadable rather than as oversize; only a whole document one byte over
// reaches the size refusal.
func publishDocumentOfBytes(t *testing.T, task string, n int) string {
	t.Helper()
	opening := strings.TrimSuffix(publishAsk(task), "}")
	padding := n - len(opening) - len("}")
	if padding < 0 {
		t.Fatalf("a %d byte document cannot hold the outcomes", n)
	}
	document := opening + strings.Repeat(" ", padding) + "}"
	if len(document) != n {
		t.Fatalf("document is %d bytes, want %d", len(document), n)
	}
	return document
}

// published runs one publish and reports what it wrote alongside its refusal.
// A refusal that wrote part of a report is worse than the refusal: the lines
// this command writes are the operator's record of what reached the commit.
func published(t *testing.T, document string) (string, error) {
	t.Helper()
	var out bytes.Buffer
	err := githubPublishCheckRuns(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

// Each App credential is named on its own when it is the one missing. The
// repository half of this is already pinned; the credentials were not, and a
// deployment that set four of five variables was being sent back through all
// of them.
func TestPublishingCheckRunsNamesTheCredentialThatIsMissing(t *testing.T) {
	for _, name := range []string{
		github.EnvAppClientID, github.EnvInstallationID,
		github.EnvPrivateKeyFile, github.EnvOwner,
	} {
		t.Run(name, func(t *testing.T) {
			task := publishCheckRunEnv(t)
			t.Setenv(name, "")
			if err := os.Unsetenv(name); err != nil {
				t.Fatal(err)
			}

			wrote, err := published(t, publishAsk(task))
			if err == nil {
				t.Fatalf("a publish missing %s answered: %q", name, wrote)
			}
			if !strings.Contains(err.Error(), name) {
				t.Fatalf("refusal=%v; want %s named", err, name)
			}
			if wrote != "" {
				t.Fatalf("a refused publish wrote %q", wrote)
			}
		})
	}
}

// A plan that cannot be published is refused as a plan, with nothing built
// and nothing posted. Each of these is a document an operator can fix; a
// refusal naming the App key instead would send them to the deployment.
func TestAPlanThatCannotBePublishedIsRefusedBeforeTheKey(t *testing.T) {
	overLongSummary := strings.Repeat("x", 70000)
	cases := map[string]func(task string) string{
		"one task named twice": func(task string) string {
			return `{"head_sha":"` + shadowCompareHead + `","runs":[` +
				`{"task":"` + task + `","state":"succeeded"},` +
				`{"task":"` + task + `","state":"failed"}]}`
		},
		"a state nothing observed": func(task string) string {
			return `{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"` + task + `","state":"banana"}]}`
		},
		"a state that has not finished": func(task string) string {
			return `{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"` + task + `","state":"running"}]}`
		},
		"a commit that is not a commit": func(task string) string {
			return `{"head_sha":"not-a-sha","runs":[{"task":"` + task + `","state":"succeeded"}]}`
		},
		"a summary GitHub will not take": func(task string) string {
			return `{"head_sha":"` + shadowCompareHead + `","runs":[{"task":"` + task +
				`","state":"succeeded","summary":"` + overLongSummary + `"}]}`
		},
	}
	for name, build := range cases {
		t.Run(name, func(t *testing.T) {
			task := publishCheckRunEnv(t)
			wrote, err := published(t, build(task))
			if err == nil {
				t.Fatalf("an unpublishable plan was accepted, and wrote %q", wrote)
			}
			if !strings.Contains(err.Error(), "plan check runs") {
				t.Fatalf("refusal=%v; want the plan named", err)
			}
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal=%v; an unpublishable plan reached the App key", err)
			}
			if wrote != "" {
				t.Fatalf("a refused publish wrote %q", wrote)
			}
		})
	}
}

// A document that plans cleanly is carried past the plan and stops at the App
// key, which is the only evidence available here that the reading and
// planning halves accepted it.
func TestAPublishableDocumentReachesTheAppKey(t *testing.T) {
	task := publishCheckRunEnv(t)
	wrote, err := published(t, publishAsk(task))
	if err == nil {
		t.Fatalf("a publish answered with no key on disk: %q", wrote)
	}
	if !strings.Contains(err.Error(), "GitHub App private key") {
		t.Fatalf("refusal=%v; want the key named", err)
	}
	if wrote != "" {
		t.Fatalf("a refused publish wrote %q", wrote)
	}
}

// The document bound is exact on both sides, and a document over it is
// refused by its own size rather than by whatever the remainder parsed as.
func TestThePublishDocumentBoundIsExact(t *testing.T) {
	t.Run("at the bound", func(t *testing.T) {
		task := publishCheckRunEnv(t)
		wrote, err := published(t, publishDocumentOfBytes(t, task, maxCheckRunPublishBytes))
		if err == nil {
			t.Fatalf("a publish answered with no key on disk: %q", wrote)
		}
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; a document at the bound was not accepted", err)
		}
	})
	t.Run("one byte over", func(t *testing.T) {
		task := publishCheckRunEnv(t)
		wrote, err := published(t, publishDocumentOfBytes(t, task, maxCheckRunPublishBytes+1))
		if err == nil {
			t.Fatalf("an oversize publish answered: %q", wrote)
		}
		for _, want := range []string{"read task outcomes", "larger than"} {
			if !strings.Contains(err.Error(), want) {
				t.Fatalf("refusal=%v; want %q named", err, want)
			}
		}
		if wrote != "" {
			t.Fatalf("a refused publish wrote %q", wrote)
		}
	})
}
