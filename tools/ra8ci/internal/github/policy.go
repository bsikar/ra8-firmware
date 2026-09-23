package github

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

// Policy limits scale-set jobs to a repository, trusted workflow references,
// allowed triggers, and dedicated labels. It is immutable after construction.
// GitHub workflow policy must still restrict who can push to those refs.
type Policy struct {
	owner        string
	repository   string
	workflowRefs map[string]struct{}
	events       map[string]struct{}
	jobNames     map[string]struct{}
	labels       map[string]struct{}
}

// NewPolicy accepts complete workflow references such as
// owner/repo/.github/workflows/ci.yml@refs/heads/dev, not path prefixes.
func NewPolicy(owner, repository string, workflowRefs, events, jobNames, labels []string) (*Policy, error) {
	if owner == "" || repository == "" || strings.ContainsAny(owner+repository, "/@ \t\n") {
		return nil, errors.New("github policy needs a single owner and repository")
	}
	if len(workflowRefs) == 0 || len(events) == 0 || len(jobNames) == 0 || len(labels) == 0 {
		return nil, errors.New("github policy requires workflow, event, job name and label allowlists")
	}
	p := &Policy{owner: owner, repository: repository, workflowRefs: make(map[string]struct{}), events: make(map[string]struct{}), jobNames: make(map[string]struct{}), labels: make(map[string]struct{})}
	for _, ref := range workflowRefs {
		if !strings.HasPrefix(ref, owner+"/"+repository+"/.github/workflows/") || !strings.Contains(ref, "@refs/heads/") || strings.ContainsAny(ref, " \t\n") {
			return nil, fmt.Errorf("invalid trusted workflow reference %q", ref)
		}
		p.workflowRefs[ref] = struct{}{}
	}
	for _, event := range events {
		if event == "" || strings.ContainsAny(event, " \t\n") {
			return nil, fmt.Errorf("invalid event %q", event)
		}
		p.events[event] = struct{}{}
	}
	for _, name := range jobNames {
		if name == "" || len(name) > 256 || strings.TrimSpace(name) != name || strings.ContainsAny(name, "\r\n\x00") {
			return nil, fmt.Errorf("invalid job name %q", name)
		}
		p.jobNames[name] = struct{}{}
	}
	if len(p.jobNames) == 0 {
		return nil, errors.New("github policy has no unique job names")
	}
	for _, label := range labels {
		if label == "" || strings.ContainsAny(label, " \t\n") {
			return nil, fmt.Errorf("invalid runner label %q", label)
		}
		p.labels[label] = struct{}{}
	}
	return p, nil
}

// Allow rejects an incomplete or untrusted job before it is acknowledged or
// acquired. It does not execute any command derived from the message.
func (p *Policy) Allow(_ context.Context, job Job) error {
	if p == nil {
		return errors.New("missing github policy")
	}
	if job.Owner != p.owner || job.Repository != p.repository || job.RunnerRequestID <= 0 || job.JobID == "" || job.WorkflowRunID <= 0 {
		return errors.New("github job identity does not match trusted repository")
	}
	if _, ok := p.workflowRefs[job.WorkflowRef]; !ok {
		return fmt.Errorf("untrusted workflow ref %q", job.WorkflowRef)
	}
	if _, ok := p.events[job.EventName]; !ok {
		return fmt.Errorf("untrusted github event %q", job.EventName)
	}
	if _, ok := p.jobNames[job.DisplayName]; !ok {
		return fmt.Errorf("untrusted github job name %q", job.DisplayName)
	}
	if len(job.Labels) == 0 {
		return errors.New("github job has no runner label")
	}
	for _, label := range job.Labels {
		if _, ok := p.labels[label]; !ok {
			return fmt.Errorf("untrusted runner label %q", label)
		}
	}
	return nil
}
