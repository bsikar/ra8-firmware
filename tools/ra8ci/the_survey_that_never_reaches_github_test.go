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

// `pull-request-survey` is the one reading subcommand that cannot answer
// without reaching GitHub: the report it writes is what GitHub said each pull
// request is at. Everything it decides BEFORE the first request is therefore
// the whole of what this box can pin, and it is the half that matters most,
// because each refusal below is one an operator meets instead of a token being
// minted against a survey nobody could have used.
//
// publishCheckRunEnv configures a complete publishing environment whose
// private key file does not exist, so a survey that gets as far as building a
// reader refuses there. That absent key is the marker this file reads
// ordering from: a refusal naming the key means the ask was accepted, and a
// refusal naming the ask means the key was never opened.

// surveyAsk is an ordinary one-candidate ask, the shape every test here
// varies from.
const surveyAsk = `{"workflow":"Checks","pull_requests":[1589]}`

// surveyDocumentOfBytes builds a valid one-candidate ask padded with
// insignificant whitespace to exactly n bytes, which is how the size bound is
// approached from both sides without a document the decoder would refuse for
// some other reason.
func surveyDocumentOfBytes(t *testing.T, n int) string {
	t.Helper()
	const opening = `{"workflow":"Checks","pull_requests":[1589]`
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

// surveyed runs one survey and reports what it wrote alongside its refusal,
// since a refusal that wrote a partial report is a worse failure than the
// refusal itself.
func surveyed(t *testing.T, document string) (string, error) {
	t.Helper()
	var out strings.Builder
	err := githubPullRequestSurvey(context.Background(), strings.NewReader(document), &out)
	return out.String(), err
}

// A correspondence on its own is not enough to survey: the pull requests live
// in a repository, and a process that knows which workflow carries evidence
// but not which repository to read cannot be half-configured into surveying
// the wrong one.
func TestASurveyNeedsARepositoryToReadPullRequestsFrom(t *testing.T) {
	shadowCompareEnv(t, "build")
	t.Setenv(github.EnvCheckRunRepository, "")
	if err := os.Unsetenv(github.EnvCheckRunRepository); err != nil {
		t.Fatal(err)
	}

	wrote, err := surveyed(t, surveyAsk)
	if err == nil {
		t.Fatalf("a survey with no repository answered: %q", wrote)
	}
	if !strings.Contains(err.Error(), github.EnvCheckRunRepository) {
		t.Fatalf("refusal=%v; want the repository variable named", err)
	}
	if wrote != "" {
		t.Fatalf("a refused survey wrote %q", wrote)
	}
}

// Each credential is named on its own when it is the one missing. A survey
// refused with "not configured" over a deployment that set four of five
// variables sends an operator back through all of them.
func TestASurveyNamesTheCredentialThatIsMissing(t *testing.T) {
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

			wrote, err := surveyed(t, surveyAsk)
			if err == nil {
				t.Fatalf("a survey missing %s answered: %q", name, wrote)
			}
			if !strings.Contains(err.Error(), name) {
				t.Fatalf("refusal=%v; want %s named", err, name)
			}
			if wrote != "" {
				t.Fatalf("a refused survey wrote %q", wrote)
			}
		})
	}
}

// The ask is judged before the App key is opened, so a document nothing could
// be surveyed from is reported as a bad ask rather than as a deployment
// problem. The absent key file is what makes the ordering visible: a refusal
// that names it is one that accepted the ask first.
func TestASurveyJudgesTheAskBeforeItOpensTheAppKey(t *testing.T) {
	t.Run("a bad ask never reaches the key", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := surveyed(t, `{"workflow":"Checks","pull_requests":[]}`)
		if err == nil {
			t.Fatalf("an empty ask answered: %q", wrote)
		}
		if !strings.Contains(err.Error(), "pull requests to survey") {
			t.Fatalf("refusal=%v; want the ask named", err)
		}
		if strings.Contains(err.Error(), "private key") {
			t.Fatalf("refusal=%v; the key was opened over a bad ask", err)
		}
		if wrote != "" {
			t.Fatalf("a refused survey wrote %q", wrote)
		}
	})
	t.Run("an accepted ask reaches the key", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := surveyed(t, surveyAsk)
		if err == nil {
			t.Fatalf("a survey answered with no key on disk: %q", wrote)
		}
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; want the key named", err)
		}
		if wrote != "" {
			t.Fatalf("a refused survey wrote %q", wrote)
		}
	})
}

// The document bound is exact on both sides, and a document over it is
// refused by its own size rather than by whatever the truncated remainder
// happened to parse as.
func TestTheSurveyDocumentBoundIsExact(t *testing.T) {
	t.Run("at the bound", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := surveyed(t, surveyDocumentOfBytes(t, maxPullRequestSurveyBytes))
		if err == nil {
			t.Fatalf("a survey answered with no key on disk: %q", wrote)
		}
		// Reaching the key is the pin: a document exactly at the
		// bound was read whole and accepted.
		if !strings.Contains(err.Error(), "GitHub App private key") {
			t.Fatalf("refusal=%v; a document at the bound was not accepted", err)
		}
	})
	t.Run("one byte over", func(t *testing.T) {
		publishCheckRunEnv(t)
		wrote, err := surveyed(t, surveyDocumentOfBytes(t, maxPullRequestSurveyBytes+1))
		if err == nil {
			t.Fatalf("an oversize survey answered: %q", wrote)
		}
		for _, want := range []string{"pull requests to survey", "larger than"} {
			if !strings.Contains(err.Error(), want) {
				t.Fatalf("refusal=%v; want %q named", err, want)
			}
		}
		if wrote != "" {
			t.Fatalf("a refused survey wrote %q", wrote)
		}
	})
}
