// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// githubCommand reads os.Stdin directly, so the only way to drive a
// subcommand the way an operator types it is to hand the process a different
// stdin. Everything below goes through githubCommand rather than calling the
// inner function, because the seam under test is the dispatch table: a
// subcommand wired to the wrong function, or to a nil one, is invisible to a
// test that calls the function itself.

// fed runs one invocation with stdin replaced by a document, which is how
// every reading subcommand is handed its input.
func fed(t *testing.T, document string, run func() error) error {
	t.Helper()
	path := filepath.Join(t.TempDir(), "document.json")
	if err := os.WriteFile(path, []byte(document), 0o600); err != nil {
		t.Fatal(err)
	}
	handle, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := handle.Close(); err != nil {
			t.Fatal(err)
		}
	}()
	saved := os.Stdin
	os.Stdin = handle
	defer func() { os.Stdin = saved }()
	return run()
}

// withoutTheGitHubEnvironment leaves every GitHub variable genuinely unset
// rather than set to an empty string. The difference decides which refusal is
// reached: unset is "not configured", while set-but-empty is a configuration
// the loader refuses as incomplete.
func withoutTheGitHubEnvironment(t *testing.T) {
	t.Helper()
	for _, name := range []string{
		github.EnvShadowCorrespondenceFile, github.EnvCheckRunMode,
		github.EnvCheckRunRepository, github.EnvConfigURL, github.EnvAppClientID,
		github.EnvInstallationID, github.EnvPrivateKeyFile, github.EnvOwner,
		github.EnvScaleSetID, github.EnvMaxRunners,
	} {
		t.Setenv(name, "placeholder")
		if err := os.Unsetenv(name); err != nil {
			t.Fatal(err)
		}
	}
}

func TestTheGitHubSubcommandsThatNeedPublishingConfiguredSayWhichVariableIsMissing(t *testing.T) {
	withoutTheGitHubEnvironment(t)
	// These four read the check-run configuration before they read anything
	// else, so an unconfigured process has to be told which variable to set
	// rather than shown an empty answer it might publish.
	for _, name := range []string{"shadow", "shadow-compare", "required-checks", "evidence-gate"} {
		t.Run(name, func(t *testing.T) {
			spoke, err := spoken(t, func() error {
				return fed(t, "{}", func() error {
					return githubCommand(context.Background(), []string{name})
				})
			})
			if err == nil {
				t.Fatalf("github %s answered without publishing configured: %q", name, spoke)
			}
			if !strings.Contains(err.Error(), github.EnvShadowCorrespondenceFile) {
				t.Fatalf("github %s: refusal does not name the variable: %v", name, err)
			}
		})
	}
}

func TestTheGitHubCheckSubcommandSaysTheIntegrationIsNotConfigured(t *testing.T) {
	withoutTheGitHubEnvironment(t)
	// check is the one subcommand that opens a session rather than reading a
	// document, and with nothing configured it has to refuse before it dials.
	spoke, err := spoken(t, func() error {
		return githubCommand(context.Background(), []string{"check"})
	})
	if err == nil {
		t.Fatalf("github check answered with no integration configured: %q", spoke)
	}
	if !strings.Contains(err.Error(), "not configured") {
		t.Fatalf("refusal=%v; want the integration named as unconfigured", err)
	}
}

func TestEveryGitHubSubcommandThatReadsADocumentRefusesOneItCannotRead(t *testing.T) {
	withoutTheGitHubEnvironment(t)
	// These four are pure document readers: no configuration, no network. An
	// unreadable document has to be a refusal rather than an empty report,
	// since an empty report from a gate reads as a pass.
	for _, name := range []string{
		"shadow-evidence", "evidence-page", "reconcile-page", "pull-request-survey-page",
	} {
		for _, document := range []struct {
			what string
			body string
		}{
			{"nothing at all", ""},
			{"a truncated object", "{"},
			{"a document of the wrong shape", `["not an object"]`},
		} {
			t.Run(name+"/"+document.what, func(t *testing.T) {
				spoke, err := spoken(t, func() error {
					return fed(t, document.body, func() error {
						return githubCommand(context.Background(), []string{name})
					})
				})
				if err == nil {
					t.Fatalf("github %s accepted %s: %q", name, document.what, spoke)
				}
			})
		}
	}
}
