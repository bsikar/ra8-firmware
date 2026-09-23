package github

import (
	"context"
	"testing"

	"github.com/actions/scaleset"
)

func TestPolicyAllowsOnlyTrustedWorkflowAndEvent(t *testing.T) {
	ref := "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"
	policy, err := NewPolicy("bsikar", "ra8-firmware", []string{ref}, []string{"push"}, []string{"CI / test-go"}, []string{"ra8ci-linux"})
	if err != nil {
		t.Fatal(err)
	}
	job := Job{Kind: scaleset.MessageTypeJobAvailable, Owner: "bsikar", Repository: "ra8-firmware", RunnerRequestID: 1, JobID: "9", WorkflowRunID: 10, WorkflowRef: ref, EventName: "push", DisplayName: "CI / test-go", Labels: []string{"ra8ci-linux"}}
	if err := policy.Allow(context.Background(), job); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name   string
		mutate func(*Job)
	}{
		{"fork", func(j *Job) { j.Owner = "attacker" }},
		{"branch", func(j *Job) { j.WorkflowRef = "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/feature" }},
		{"pull request", func(j *Job) { j.EventName = "pull_request" }},
		{"unapproved job name", func(j *Job) { j.DisplayName = "arbitrary privileged job" }},
		{"missing job name", func(j *Job) { j.DisplayName = "" }},
		{"label", func(j *Job) { j.Labels = []string{"ra8ci-windows"} }},
		{"missing run", func(j *Job) { j.WorkflowRunID = 0 }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			candidate := job
			tc.mutate(&candidate)
			if err := policy.Allow(context.Background(), candidate); err == nil {
				t.Fatal("untrusted job accepted")
			}
		})
	}
}

func TestPolicyRejectsPrefixInsteadOfExactRef(t *testing.T) {
	if _, err := NewPolicy("bsikar", "ra8-firmware", []string{"bsikar/ra8-firmware/.github/workflows/ci.yml"}, []string{"push"}, []string{"CI / test-go"}, []string{"ra8ci-linux"}); err == nil {
		t.Fatal("workflow prefix accepted")
	}
}

func TestPolicyRequiresJobNameAllowlist(t *testing.T) {
	ref := "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev"
	if _, err := NewPolicy("bsikar", "ra8-firmware", []string{ref}, []string{"push"}, nil, []string{"ra8ci-linux"}); err == nil {
		t.Fatal("empty job-name allowlist accepted")
	}
	for _, name := range []string{"", " leading", "trailing ", "line\nbreak"} {
		if _, err := NewPolicy("bsikar", "ra8-firmware", []string{ref}, []string{"push"}, []string{name}, []string{"ra8ci-linux"}); err == nil {
			t.Fatalf("invalid job name %q accepted", name)
		}
	}
}
