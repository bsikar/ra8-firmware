// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// `evidence-run` reads a pull request number and a workflow name, and answers
// with the one run on that pull request's head whose job conclusions are the
// Actions half of a shadow comparison. Everything it decides before the first
// request is what a box with no App key can pin, and it is the half an
// operator meets: a bad ask has to be reported as a bad ask rather than as a
// failure to reach GitHub.
//
// The environment matters as much as the document here. publishCheckRunEnv
// configures a COMPLETE publishing environment whose private key file does
// not exist, so a command that gets as far as building a reader refuses
// there. That absent key is the ordering marker every test below reads: a
// refusal naming the key accepted the ask first, and a refusal naming the ask
// never opened the key. A test run under a half-configured environment would
// be refused for the missing repository and never reach the document at all,
// which is a refusal that says nothing about the ask.

// evidenceRunAsk is an ordinary one-pull-request ask.
const evidenceRunAsk = `{"number":1592,"workflow":"Checks"}`

// evidenceRunDocumentOfBytes builds a valid ask padded with insignificant
// whitespace to exactly n bytes, which is how the size bound is approached
// from both sides without a document the decoder would refuse for some other
// reason. The decoder runs under a limit reader of the bound plus one, so a
// document far over it is truncated and refused as an unreadable document
// rather than as an oversize one.
func evidenceRunDocumentOfBytes(t *testing.T, n int) string {
	t.Helper()
	const opening = `{"number":1592,"workflow":"Checks"`
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

// askedForARun runs one selection and reports what it wrote alongside its
// refusal, since a refusal that wrote a partial report is worse than the
// refusal itself.
func askedForARun(t *testing.T, document string) (string, error) {
	t.Helper()
	var out strings.Builder
	err := githubEvidenceRun(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

// A correspondence on its own is not enough: the pull request lives in a
// repository, and a process that knows which workflow carries evidence but
// not which repository to read it from is refused by name.
func TestSelectingAnEvidenceRunNeedsARepository(t *testing.T) {
	shadowCompareEnv(t, "build")
	t.Setenv(github.EnvCheckRunRepository, "")
	if err := os.Unsetenv(github.EnvCheckRunRepository); err != nil {
		t.Fatal(err)
	}

	wrote, err := askedForARun(t, evidenceRunAsk)
	if err == nil {
		t.Fatalf("a selection with no repository answered: %q", wrote)
	}
	if !strings.Contains(err.Error(), github.EnvCheckRunRepository) {
		t.Fatalf("refusal=%v; want the repository variable named", err)
	}
	if wrote != "" {
		t.Fatalf("a refused selection wrote %q", wrote)
	}
}

// Each App credential is named on its own when it is the one missing, rather
// than a deployment that set four of five variables being sent back through
// all of them.
func TestSelectingAnEvidenceRunNamesTheCredentialThatIsMissing(t *testing.T) {
	for _, name := range []string{
		github.EnvAppClientID, github.EnvInstallationID,
		github.EnvPrivateKeyFile, github.EnvOwner,
	} {
		t.Run(name, func(t *testing.T) {
			publishCheckRunEnv(t)
			t.Setenv(name, "")
			if err := os.Unsetenv(name); err != nil {
				t.Fatal(err)
			}

			wrote, err := askedForARun(t, evidenceRunAsk)
			if err == nil {
				t.Fatalf("a selection missing %s answered: %q", name, wrote)
			}
			if !strings.Contains(err.Error(), name) {
				t.Fatalf("refusal=%v; want %s named", err, name)
			}
			if wrote != "" {
				t.Fatalf("a refused selection wrote %q", wrote)
			}
		})
	}
}

// Every unreadable ask is refused AS an ask, under an environment that would
// have let a good one through. The second assertion is the one that carries
// the weight: a refusal naming the private key would mean the document was
// accepted and the failure came later, and a refusal naming a variable would
// mean the document was never read at all.
func TestAnEvidenceRunAskIsRefusedInItsOwnWords(t *testing.T) {
	cases := map[string]string{
		"not an object":     `["1592"]`,
		"unknown field":     `{"number":1592,"workflow":"Checks","run_id":9}`,
		"trailing document": `{"number":1592,"workflow":"Checks"}{"number":1593,"workflow":"Checks"}`,
		"no number":         `{"workflow":"Checks"}`,
		"zero number":       `{"number":0,"workflow":"Checks"}`,
		"negative number":   `{"number":-1,"workflow":"Checks"}`,
		"no workflow":       `{"number":1592}`,
		"blank workflow":    `{"number":1592,"workflow":"   "}`,
	}
	for name, document := range cases {
		t.Run(name, func(t *testing.T) {
			publishCheckRunEnv(t)
			wrote, err := askedForARun(t, document)
			if err == nil {
				t.Fatalf("the ask was not refused, and wrote %q", wrote)
			}
			if !strings.Contains(err.Error(), "read the run to select") {
				t.Fatalf("refusal=%v; want the ask named", err)
			}
			if strings.Contains(err.Error(), "private key") {
				t.Fatalf("refusal=%v; a bad ask reached the App key", err)
			}
			if wrote != "" {
				t.Fatalf("a refused ask wrote %q", wrote)
			}
		})
	}
}

// An ask this command can read is carried past the document and stops at the
// App key, which is the only evidence available here that the reading half
// accepted it.
func TestAReadableEvidenceRunAskReachesTheAppKey(t *testing.T) {
	publishCheckRunEnv(t)
	wrote, err := askedForARun(t, evidenceRunAsk)
	if err == nil {
		t.Fatalf("a selection answered with no key on disk: %q", wrote)
	}
	if !strings.Contains(err.Error(), "GitHub App private key") {
		t.Fatalf("refusal=%v; want the key named", err)
	}
	if wrote != "" {
		t.Fatalf("a refused selection wrote %q", wrote)
	}
}

// The document bound is exact on both sides, and a document over it is
// refused by its own size rather than by whatever the remainder parsed as.
func TestTheEvidenceRunDocumentBoundIsExact(t *testing.T) {
	t.Run("at the bound", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := askedForARun(t, evidenceRunDocumentOfBytes(t, maxEvidenceRunRequestBytes))
		if err == nil {
			t.Fatalf("a selection answered with no key on disk: %q", wrote)
		}
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; a document at the bound was not accepted", err)
		}
	})
	t.Run("one byte over", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := askedForARun(t, evidenceRunDocumentOfBytes(t, maxEvidenceRunRequestBytes+1))
		if err == nil {
			t.Fatalf("an oversize selection answered: %q", wrote)
		}
		for _, want := range []string{"read the run to select", "larger than"} {
			if !strings.Contains(err.Error(), want) {
				t.Fatalf("refusal=%v; want %q named", err, want)
			}
		}
		if wrote != "" {
			t.Fatalf("a refused selection wrote %q", wrote)
		}
	})
}
