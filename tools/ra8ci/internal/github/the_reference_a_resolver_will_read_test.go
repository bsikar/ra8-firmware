// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"strings"
	"testing"
)

// A workflow reference is what says which file, on which branch, produced
// this job. Everything the resolver goes on to fetch is derived from it, so
// a reference that is nearly right is worse than one that is plainly wrong:
// it would resolve, against a file the workflow never ran.

// Only an exact branch reference to a workflow file inside this repository
// is read, and each half is refused on its own terms.
func TestOnlyAnExactBranchWorkflowReferenceIsRead(t *testing.T) {
	const owner, repository = "bsikar", "ra8-firmware"
	sound := "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"

	workflowPath, branch, ok := parseWorkflowRef(sound, owner, repository)
	if !ok || workflowPath != ".github/workflows/ci.yml" || branch != "dev" {
		t.Fatalf("parseWorkflowRef(%q) = %q, %q, %v", sound, workflowPath, branch, ok)
	}

	// A branch carrying slashes is ordinary and is kept whole.
	if _, branch, ok = parseWorkflowRef(
		"bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/ra8ci/dev", owner, repository); !ok || branch != "ra8ci/dev" {
		t.Fatalf("a nested branch answered %q, %v", branch, ok)
	}

	for _, bad := range []struct {
		name string
		ref  string
	}{
		{"another repository", "attacker/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"},
		{"another owner's lookalike", "bsikar-evil/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"},
		{"no reference at all", ""},
		{"no ref half", "bsikar/ra8-firmware/.github/workflows/ci.yml"},
		{"outside the workflows directory", "bsikar/ra8-firmware/scripts/ci.yml@refs/heads/dev"},
		{"a tag", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/tags/v1"},
		{"a pull request merge ref", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/pull/12/merge"},
		{"a bare sha", "bsikar/ra8-firmware/.github/workflows/ci.yml@0123456789abcdef0123456789abcdef01234567"},
		{"a traversal", "bsikar/ra8-firmware/.github/workflows/../../etc/passwd@refs/heads/dev"},
		{"a traversal inside the directory", "bsikar/ra8-firmware/.github/workflows/../workflows/ci.yml@refs/heads/dev"},
		{"an unclean path", "bsikar/ra8-firmware/.github/workflows/./ci.yml@refs/heads/dev"},
		{"a doubled separator", "bsikar/ra8-firmware/.github/workflows//ci.yml@refs/heads/dev"},
		{"a trailing separator", "bsikar/ra8-firmware/.github/workflows/ci.yml/@refs/heads/dev"},
		{"no branch", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/"},
		{"a spaced branch", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev branch"},
		{"a tabbed branch", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev\tbranch"},
		{"a branch carrying a return", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev\r"},
		{"a branch carrying a newline", "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev\n"},
	} {
		if workflowPath, branch, ok := parseWorkflowRef(bad.ref, owner, repository); ok {
			t.Fatalf("%s was read as %q on %q", bad.name, workflowPath, branch)
		}
	}
}

// The resolver is the only thing that turns a scale-set message into run
// metadata the plane trusts, so an unusable configuration has to be refused
// at the build rather than at the first job, when a runner is already
// waiting on the answer.
func TestAnUnusableMetadataConfigurationIsRefusedAtTheBuild(t *testing.T) {
	keyPath := soundKeyFile(t)
	sound := MetadataConfig{
		APIBaseURL: "https://api.github.com", AppClientID: "client-id", InstallationID: 42,
		PrivateKeyFile: keyPath, Owner: "bsikar", Repository: "ra8-firmware",
	}
	if _, err := NewMetadataResolver(sound); err != nil {
		t.Fatalf("a sound configuration was refused: %v", err)
	}

	for _, bad := range []struct {
		name  string
		amend func(*MetadataConfig)
	}{
		{"no client id", func(c *MetadataConfig) { c.AppClientID = "" }},
		{"an overlong client id", func(c *MetadataConfig) { c.AppClientID = strings.Repeat("c", 257) }},
		{"no installation", func(c *MetadataConfig) { c.InstallationID = 0 }},
		{"a negative installation", func(c *MetadataConfig) { c.InstallationID = -1 }},
		{"no key file", func(c *MetadataConfig) { c.PrivateKeyFile = "" }},
		{"no owner", func(c *MetadataConfig) { c.Owner = "" }},
		{"an owner holding a path", func(c *MetadataConfig) { c.Owner = "bsikar/evil" }},
		{"an owner holding a space", func(c *MetadataConfig) { c.Owner = "bsikar evil" }},
		{"no repository", func(c *MetadataConfig) { c.Repository = "" }},
		{"a repository holding a path", func(c *MetadataConfig) { c.Repository = "ra8-firmware/evil" }},
		{"a repository holding a newline", func(c *MetadataConfig) { c.Repository = "ra8-firmware\n" }},
	} {
		candidate := sound
		bad.amend(&candidate)
		resolver, err := NewMetadataResolver(candidate)
		if err == nil || resolver != nil {
			t.Fatalf("%s built a resolver: %v", bad.name, err)
		}
		if !strings.Contains(err.Error(), "invalid GitHub metadata resolver configuration") {
			t.Fatalf("%s was refused as something else: %v", bad.name, err)
		}
	}

	// A key file that is not a key is refused by the key loader, which
	// names the file rather than the configuration around it.
	for _, keyFile := range badKeyFiles(t) {
		candidate := sound
		candidate.PrivateKeyFile = keyFile
		resolver, err := NewMetadataResolver(candidate)
		if err == nil || resolver != nil {
			t.Fatalf("key file %q built a resolver: %v", keyFile, err)
		}
		if strings.Contains(err.Error(), "invalid GitHub metadata resolver configuration") {
			t.Fatalf("key file %q was refused as a configuration problem: %v", keyFile, err)
		}
	}
}
