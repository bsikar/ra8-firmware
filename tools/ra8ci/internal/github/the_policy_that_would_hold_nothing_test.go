// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"strings"
	"testing"

	"github.com/actions/scaleset"
)

// The policy is the only thing standing between a scale set and a workflow
// that would run arbitrary steps on it, so what it refuses to be BUILT from
// matters as much as what it refuses to admit. A policy that accepted a
// smuggled owner or a blank label would look like a policy and hold nothing.

// trusted is a policy over one workflow, one event, one job name and one
// label, the shape the ra8ci scale set actually runs under.
func trusted(t *testing.T) (*Policy, string) {
	t.Helper()
	ref := "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"
	policy, err := NewPolicy("bsikar", "ra8-firmware",
		[]string{ref}, []string{"push"}, []string{"CI / test-go"}, []string{"ra8ci-linux"})
	if err != nil {
		t.Fatal(err)
	}
	return policy, ref
}

// An owner or repository carrying a separator is a second identity smuggled
// into the first, and the reference check downstream is built by pasting
// both together. Neither may hold one.
func TestAnOwnerOrRepositoryThatIsReallyTwoIsRefused(t *testing.T) {
	for _, bad := range []struct {
		name       string
		owner      string
		repository string
	}{
		{"no owner", "", "ra8-firmware"},
		{"no repository", "bsikar", ""},
		{"neither", "", ""},
		{"owner holds a path", "bsikar/evil", "ra8-firmware"},
		{"repository holds a path", "bsikar", "ra8-firmware/evil"},
		{"owner holds a ref", "bsikar@refs", "ra8-firmware"},
		{"repository holds a ref", "bsikar", "ra8-firmware@refs/heads/dev"},
		{"owner holds a space", "bsikar ", "ra8-firmware"},
		{"repository holds a tab", "bsikar", "ra8\t-firmware"},
		{"repository holds a newline", "bsikar", "ra8-firmware\n"},
	} {
		built, err := NewPolicy(bad.owner, bad.repository,
			[]string{"bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"},
			[]string{"push"}, []string{"CI / test-go"}, []string{"ra8ci-linux"})
		if err == nil || built != nil {
			t.Fatalf("%s built a policy: %v", bad.name, err)
		}
		if !strings.Contains(err.Error(), "single owner and repository") {
			t.Fatalf("%s was refused as something else: %v", bad.name, err)
		}
	}
}

// An allowlist entry that is blank or carries whitespace never matches the
// value GitHub sends, so it is dead weight in a list whose whole job is to
// be matched against. Each is named at construction rather than carried.
func TestAnAllowlistEntryThatCouldNeverMatchIsRefused(t *testing.T) {
	ref := "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"
	for _, bad := range []struct {
		name     string
		events   []string
		jobNames []string
		labels   []string
		says     string
	}{
		{name: "blank event", events: []string{""}, says: "invalid event"},
		{name: "spaced event", events: []string{"pull request"}, says: "invalid event"},
		{name: "tabbed event", events: []string{"push\t"}, says: "invalid event"},
		{name: "newline event", events: []string{"push\n"}, says: "invalid event"},
		{name: "blank job name", jobNames: []string{""}, says: "invalid job name"},
		{name: "untrimmed job name", jobNames: []string{" CI / test-go"}, says: "invalid job name"},
		{name: "job name with a return", jobNames: []string{"CI / test-go\r"}, says: "invalid job name"},
		{name: "job name with a NUL", jobNames: []string{"CI / test-go\x00"}, says: "invalid job name"},
		{name: "overlong job name", jobNames: []string{strings.Repeat("j", 257)}, says: "invalid job name"},
		{name: "blank label", labels: []string{""}, says: "invalid runner label"},
		{name: "spaced label", labels: []string{"ra8ci linux"}, says: "invalid runner label"},
		{name: "tabbed label", labels: []string{"ra8ci-linux\t"}, says: "invalid runner label"},
		{name: "newline label", labels: []string{"ra8ci-linux\n"}, says: "invalid runner label"},
	} {
		events, jobNames, labels := bad.events, bad.jobNames, bad.labels
		if events == nil {
			events = []string{"push"}
		}
		if jobNames == nil {
			jobNames = []string{"CI / test-go"}
		}
		if labels == nil {
			labels = []string{"ra8ci-linux"}
		}
		built, err := NewPolicy("bsikar", "ra8-firmware", []string{ref}, events, jobNames, labels)
		if err == nil || built != nil {
			t.Fatalf("%s built a policy: %v", bad.name, err)
		}
		if !strings.Contains(err.Error(), bad.says) {
			t.Fatalf("%s was refused as something else: %v", bad.name, err)
		}
	}

	// A job name of exactly the 256-byte bound is the longest GitHub will
	// send, and it is admitted rather than rounded off.
	longest := strings.Repeat("j", 256)
	if _, err := NewPolicy("bsikar", "ra8-firmware", []string{ref},
		[]string{"push"}, []string{longest}, []string{"ra8ci-linux"}); err != nil {
		t.Fatalf("a job name at the bound was refused: %v", err)
	}
}

// A policy that was never built refuses everything. The controller holds an
// Admission interface, so a nil *Policy arrives as a non-nil admission that
// would otherwise dereference nothing and admit by accident.
func TestAPolicyThatWasNeverBuiltAdmitsNothing(t *testing.T) {
	var missing *Policy
	_, ref := trusted(t)
	job := Job{
		Kind: scaleset.MessageTypeJobAvailable, Owner: "bsikar", Repository: "ra8-firmware",
		RunnerRequestID: 1, JobID: "9", WorkflowRunID: 10, WorkflowRef: ref,
		EventName: "push", DisplayName: "CI / test-go", Labels: []string{"ra8ci-linux"},
	}
	err := missing.Allow(context.Background(), job)
	if err == nil || !strings.Contains(err.Error(), "missing github policy") {
		t.Fatalf("a job under no policy answered %v", err)
	}

	var admission Admission = missing
	if err := admission.Allow(context.Background(), job); err == nil {
		t.Fatal("a job was admitted through a nil policy held as an admission")
	}
}

// A job carrying no label at all is refused as the labelless job it is,
// rather than passing the per-label check vacuously. Without a label the
// scale set has nothing to route the job to.
func TestAJobWithNoLabelIsRefusedRatherThanPassingVacuously(t *testing.T) {
	policy, ref := trusted(t)
	job := Job{
		Kind: scaleset.MessageTypeJobAvailable, Owner: "bsikar", Repository: "ra8-firmware",
		RunnerRequestID: 1, JobID: "9", WorkflowRunID: 10, WorkflowRef: ref,
		EventName: "push", DisplayName: "CI / test-go",
	}
	for _, labels := range [][]string{nil, {}} {
		candidate := job
		candidate.Labels = labels
		err := policy.Allow(context.Background(), candidate)
		if err == nil || !strings.Contains(err.Error(), "no runner label") {
			t.Fatalf("a job with %v labels answered %v", labels, err)
		}
	}

	// One trusted label alongside an untrusted one is still untrusted: the
	// check is over every label, not the first that matches.
	candidate := job
	candidate.Labels = []string{"ra8ci-linux", "ra8ci-windows"}
	err := policy.Allow(context.Background(), candidate)
	if err == nil || !strings.Contains(err.Error(), `untrusted runner label "ra8ci-windows"`) {
		t.Fatalf("a job carrying an extra label answered %v", err)
	}

	// And the job the policy was built for is still admitted, so none of
	// the above is the policy refusing everything.
	candidate = job
	candidate.Labels = []string{"ra8ci-linux"}
	if err := policy.Allow(context.Background(), candidate); err != nil {
		t.Fatalf("the trusted job was refused: %v", err)
	}
}
