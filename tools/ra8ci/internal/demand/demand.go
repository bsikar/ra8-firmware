// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package demand normalizes the events that tell the control plane work is
// waiting. A GitHub App delivering workflow_job and the Actions scale-set
// client are two adapters onto the same Source, so the rest of the control
// plane never sees an adapter-specific shape and no adapter-specific
// identifier can become a primary key: the identity of a unit of demand is
// the job id and the workflow run attempt, which both adapters carry.
package demand

import (
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strconv"
	"time"
)

// MaxPayloadBytes bounds one webhook delivery. GitHub's own limit is 25 MiB
// for the whole request; a workflow_job body that large is not one this
// control plane is going to understand, and parsing it is work an unverified
// sender should not be able to ask for.
const MaxPayloadBytes = 1 << 20

// MaxLabels bounds the labels a job may ask for. Placement reads them, so an
// unbounded list is an unbounded amount of matching per delivery.
const MaxLabels = 32

var (
	// ErrInvalid is a delivery this package cannot trust: malformed JSON,
	// a missing identity, a repository that does not agree with itself.
	ErrInvalid = errors.New("invalid demand event")
	// ErrIgnored is a well-formed delivery that carries no demand, such as
	// a workflow_job action this control plane does not act on. A webhook
	// endpoint should accept it, not retry it.
	ErrIgnored = errors.New("demand event ignored")
)

var (
	ownerPattern      = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9-]{0,38}$`)
	repositoryPattern = regexp.MustCompile(`^[A-Za-z0-9._-]{1,100}$`)
	labelPattern      = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._/-]{0,63}$`)
	commitPattern     = regexp.MustCompile(`^[0-9a-f]{40}$`)
	deliveryPattern   = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$`)
	runnerPattern     = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`)
)

// Phase is where a unit of demand has got to. The order matters: deliveries
// arrive out of order, so a later phase must not be overwritten by an earlier
// one that was merely delivered second.
type Phase string

const (
	PhaseQueued     Phase = "queued"
	PhaseInProgress Phase = "in_progress"
	PhaseCompleted  Phase = "completed"
)

// Rank orders the phases. A phase with a higher rank has strictly more
// information about the job than a lower one.
func (p Phase) Rank() int {
	switch p {
	case PhaseQueued:
		return 1
	case PhaseInProgress:
		return 2
	case PhaseCompleted:
		return 3
	default:
		return 0
	}
}

func (p Phase) valid() bool { return p.Rank() > 0 }

var conclusions = map[string]bool{"success": true, "failure": true, "cancelled": true,
	"skipped": true, "timed_out": true, "neutral": true, "action_required": true, "stale": true}

// Event is one unit of demand, normalized. Nothing adapter-specific is load
// bearing here: DeliveryID and Adapter are evidence about how this copy of
// the event arrived, not identity.
type Event struct {
	Adapter     string    `json:"adapter"`
	DeliveryID  string    `json:"delivery_id"`
	Phase       Phase     `json:"phase"`
	JobID       int64     `json:"job_id"`
	RunID       int64     `json:"run_id"`
	RunAttempt  int       `json:"run_attempt"`
	Owner       string    `json:"owner"`
	Repository  string    `json:"repository"`
	Workflow    string    `json:"workflow"`
	JobName     string    `json:"job_name"`
	CommitSHA   string    `json:"commit_sha"`
	Labels      []string  `json:"labels"`
	RunnerName  string    `json:"runner_name,omitempty"`
	Conclusion  string    `json:"conclusion,omitempty"`
	QueuedAt    time.Time `json:"queued_at"`
	StartedAt   time.Time `json:"started_at,omitzero"`
	CompletedAt time.Time `json:"completed_at,omitzero"`
	ObservedAt  time.Time `json:"observed_at"`
}

// Key is the identity of a unit of demand: the job and the run attempt that
// asked for it. A retried workflow run produces a new attempt and therefore a
// new unit of demand, and two deliveries of the same queued event produce the
// same key however many delivery ids GitHub spends on them.
func (e Event) Key() string {
	return strconv.FormatInt(e.JobID, 10) + "/" + strconv.Itoa(e.RunAttempt)
}

// FullRepository is the owner-qualified name, the form the API and the
// scale-set adapter both speak.
func (e Event) FullRepository() string { return e.Owner + "/" + e.Repository }

// Supersedes reports whether e should replace prior. It is the answer to an
// out-of-order delivery: the same job seen at a later phase supersedes, the
// same phase delivered twice does not, and a different unit of demand never
// does.
func (e Event) Supersedes(prior Event) bool {
	if e.Key() != prior.Key() {
		return false
	}
	return e.Phase.Rank() > prior.Phase.Rank()
}

// Validate states what every adapter has to produce, so an adapter that grows
// a new field cannot quietly emit demand the rest of the plane cannot place.
func (e Event) Validate() error {
	switch {
	case e.Adapter == "" || len(e.Adapter) > 64:
		return fmt.Errorf("%w: adapter", ErrInvalid)
	case !deliveryPattern.MatchString(e.DeliveryID):
		return fmt.Errorf("%w: delivery id", ErrInvalid)
	case !e.Phase.valid():
		return fmt.Errorf("%w: phase %q", ErrInvalid, e.Phase)
	case e.JobID <= 0 || e.RunID <= 0:
		return fmt.Errorf("%w: job or run id", ErrInvalid)
	case e.RunAttempt < 1 || e.RunAttempt > 1000:
		return fmt.Errorf("%w: run attempt", ErrInvalid)
	case !ownerPattern.MatchString(e.Owner) || !repositoryPattern.MatchString(e.Repository):
		return fmt.Errorf("%w: repository", ErrInvalid)
	case e.Workflow == "" || len(e.Workflow) > 255 || e.JobName == "" || len(e.JobName) > 255:
		return fmt.Errorf("%w: workflow or job name", ErrInvalid)
	case !commitPattern.MatchString(e.CommitSHA):
		return fmt.Errorf("%w: commit", ErrInvalid)
	case len(e.Labels) == 0 || len(e.Labels) > MaxLabels:
		return fmt.Errorf("%w: labels", ErrInvalid)
	case e.QueuedAt.IsZero() || e.ObservedAt.IsZero():
		return fmt.Errorf("%w: timestamps", ErrInvalid)
	}
	seen := make(map[string]bool, len(e.Labels))
	for _, label := range e.Labels {
		if !labelPattern.MatchString(label) || seen[label] {
			return fmt.Errorf("%w: label %q", ErrInvalid, label)
		}
		seen[label] = true
	}
	if e.RunnerName != "" && !runnerPattern.MatchString(e.RunnerName) {
		return fmt.Errorf("%w: runner name", ErrInvalid)
	}
	if e.Phase == PhaseCompleted {
		if !conclusions[e.Conclusion] {
			return fmt.Errorf("%w: conclusion %q", ErrInvalid, e.Conclusion)
		}
		if e.CompletedAt.IsZero() {
			return fmt.Errorf("%w: completion time", ErrInvalid)
		}
	} else if e.Conclusion != "" {
		return fmt.Errorf("%w: conclusion before completion", ErrInvalid)
	}
	if e.Phase != PhaseQueued && e.StartedAt.IsZero() {
		return fmt.Errorf("%w: start time", ErrInvalid)
	}
	if !e.CompletedAt.IsZero() && e.CompletedAt.Before(e.QueuedAt) {
		return fmt.Errorf("%w: completed before queued", ErrInvalid)
	}
	return nil
}

// workflowJobDelivery is the subset of the workflow_job webhook body this
// control plane reads. Everything outside it, including whatever GitHub adds
// next, is deliberately ignored rather than carried forward.
type workflowJobDelivery struct {
	Action      string `json:"action"`
	WorkflowJob struct {
		ID           int64    `json:"id"`
		RunID        int64    `json:"run_id"`
		RunAttempt   int      `json:"run_attempt"`
		Name         string   `json:"name"`
		WorkflowName string   `json:"workflow_name"`
		HeadSHA      string   `json:"head_sha"`
		Status       string   `json:"status"`
		Conclusion   *string  `json:"conclusion"`
		Labels       []string `json:"labels"`
		RunnerName   string   `json:"runner_name"`
		CreatedAt    string   `json:"created_at"`
		StartedAt    string   `json:"started_at"`
		CompletedAt  *string  `json:"completed_at"`
	} `json:"workflow_job"`
	Repository struct {
		Name     string `json:"name"`
		FullName string `json:"full_name"`
		Owner    struct {
			Login string `json:"login"`
		} `json:"owner"`
	} `json:"repository"`
}

func parseTime(value string) (time.Time, error) {
	parsed, err := time.Parse(time.RFC3339, value)
	if err != nil {
		return time.Time{}, fmt.Errorf("%w: timestamp %q", ErrInvalid, value)
	}
	return parsed.UTC(), nil
}

// NormalizeWorkflowJob turns one workflow_job delivery into one demand event.
// observedAt is the server's own receipt time, never a field of the payload:
// a sender must not be able to backdate demand. A delivery this plane does
// not act on, such as the waiting action, comes back as ErrIgnored so the
// endpoint can accept it instead of making GitHub retry it forever.
func NormalizeWorkflowJob(adapter, deliveryID string, payload []byte, observedAt time.Time) (Event, error) {
	if len(payload) == 0 || len(payload) > MaxPayloadBytes {
		return Event{}, fmt.Errorf("%w: payload size %d", ErrInvalid, len(payload))
	}
	var delivery workflowJobDelivery
	if err := json.Unmarshal(payload, &delivery); err != nil {
		return Event{}, fmt.Errorf("%w: %v", ErrInvalid, err)
	}
	phase := Phase(delivery.Action)
	if !phase.valid() {
		if delivery.Action == "" {
			return Event{}, fmt.Errorf("%w: missing action", ErrInvalid)
		}
		return Event{}, fmt.Errorf("%w: action %q", ErrIgnored, delivery.Action)
	}
	// The payload states the job's status twice. They have to agree, or the
	// delivery is not describing a state this plane can act on.
	if delivery.WorkflowJob.Status != string(phase) {
		return Event{}, fmt.Errorf("%w: action %q with status %q", ErrInvalid,
			delivery.Action, delivery.WorkflowJob.Status)
	}
	owner := delivery.Repository.Owner.Login
	name := delivery.Repository.Name
	if delivery.Repository.FullName != owner+"/"+name {
		return Event{}, fmt.Errorf("%w: repository %q does not match %s/%s", ErrInvalid,
			delivery.Repository.FullName, owner, name)
	}
	queued, err := parseTime(delivery.WorkflowJob.CreatedAt)
	if err != nil {
		return Event{}, err
	}
	event := Event{Adapter: adapter, DeliveryID: deliveryID, Phase: phase,
		JobID: delivery.WorkflowJob.ID, RunID: delivery.WorkflowJob.RunID,
		RunAttempt: delivery.WorkflowJob.RunAttempt, Owner: owner, Repository: name,
		Workflow: delivery.WorkflowJob.WorkflowName, JobName: delivery.WorkflowJob.Name,
		CommitSHA: delivery.WorkflowJob.HeadSHA, Labels: delivery.WorkflowJob.Labels,
		RunnerName: delivery.WorkflowJob.RunnerName, QueuedAt: queued,
		ObservedAt: observedAt.UTC()}
	if phase != PhaseQueued {
		if event.StartedAt, err = parseTime(delivery.WorkflowJob.StartedAt); err != nil {
			return Event{}, err
		}
	}
	if delivery.WorkflowJob.Conclusion != nil {
		event.Conclusion = *delivery.WorkflowJob.Conclusion
	}
	if delivery.WorkflowJob.CompletedAt != nil {
		if event.CompletedAt, err = parseTime(*delivery.WorkflowJob.CompletedAt); err != nil {
			return Event{}, err
		}
	}
	if err := event.Validate(); err != nil {
		return Event{}, err
	}
	return event, nil
}
