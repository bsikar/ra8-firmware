// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// maxScaleSetDemandFailures bounds how many jobs in one message this source
// will fail on before it stops. A message every job of which is unreadable is
// a condition to report, not a list to walk to the end.
const maxScaleSetDemandFailures = 8

// scaleSetConclusions maps what the scale set calls a result onto the
// conclusions the demand contract admits. An unlisted result is a per-job
// failure rather than a guess: recording the wrong conclusion is worse than
// leaving the unit of demand open for the reconciliation pass to settle.
var scaleSetConclusions = map[string]string{
	"succeeded": "success",
	"success":   "success",
	"failed":    "failure",
	"failure":   "failure",
	"canceled":  "cancelled",
	"cancelled": "cancelled",
	"skipped":   "skipped",
	"abandoned": "stale",
	"timedout":  "timed_out",
	"timed_out": "timed_out",
}

// MetadataSource binds a scale-set job to the workflow attempt and commit it
// belongs to. The scale-set message carries neither, and demand identity is
// the job plus the run attempt, so this lookup is what makes a scale-set
// observation addressable as the same unit of demand the App delivers.
type MetadataSource interface {
	Resolve(ctx context.Context, job Job) (JobMetadata, error)
}

var _ MetadataSource = (*MetadataResolver)(nil)
var _ demand.Source = (*ScaleSetSource)(nil)

// ScaleSetSource turns scale-set messages into normalized demand. It is the
// second adapter onto one contract, and it is off unless the registry was
// built naming demand.ScaleSetAdapter: a plane running the GitHub App does
// not want the same jobs arriving twice by a route nobody switched on.
type ScaleSetSource struct {
	registry *demand.Registry
	metadata MetadataSource
	now      func() time.Time
}

// ScaleSetReport is what one message did.
type ScaleSetReport struct {
	// Scanned is the jobs the message carried.
	Scanned int
	// Recorded is the units of demand the plane now holds.
	Recorded int
	// Failed is the jobs this source could not state as demand: no
	// metadata, an unreadable identity, a result outside the contract.
	Failed int
}

// NewScaleSetSource requires the gate and the metadata lookup up front, so a
// source cannot exist that writes without either.
func NewScaleSetSource(registry *demand.Registry, metadata MetadataSource) (*ScaleSetSource, error) {
	if registry == nil || metadata == nil {
		return nil, errors.New("scale-set demand source requires a registry and a metadata resolver")
	}
	return &ScaleSetSource{registry: registry, metadata: metadata, now: time.Now}, nil
}

// Adapter names the evidence this source writes.
func (s *ScaleSetSource) Adapter() string { return demand.ScaleSetAdapter }

// Enabled reports whether the plane accepts demand from the scale set.
func (s *ScaleSetSource) Enabled() bool {
	return s != nil && s.registry.Enabled(demand.ScaleSetAdapter)
}

// Observe records every job the message carries as demand. A disabled source
// reads nothing and calls nothing: the gate is checked before the metadata
// lookup, because that lookup spends the forge's rate limit.
func (s *ScaleSetSource) Observe(ctx context.Context, message Message) (ScaleSetReport, error) {
	var report ScaleSetReport
	if s == nil || ctx == nil {
		return report, fmt.Errorf("%w: scale-set demand source", demand.ErrInvalid)
	}
	if !s.Enabled() {
		return report, nil
	}
	var failure error
	buckets := []struct {
		jobs  []Job
		phase demand.Phase
	}{
		{message.Available, demand.PhaseQueued},
		{message.Assigned, demand.PhaseQueued},
		{message.Started, demand.PhaseInProgress},
		{message.Completed, demand.PhaseCompleted},
	}
	for _, bucket := range buckets {
		for _, job := range bucket.jobs {
			report.Scanned++
			event, err := s.event(ctx, message, job, bucket.phase)
			if err != nil {
				report.Failed++
				if failure == nil {
					failure = err
				}
				if report.Failed >= maxScaleSetDemandFailures {
					return report, failure
				}
				continue
			}
			if err := s.registry.Submit(ctx, event); err != nil {
				return report, fmt.Errorf("record scale-set demand %s: %w", event.Key(), err)
			}
			report.Recorded++
		}
	}
	return report, failure
}

// event states one scale-set job as demand. Identity comes from the job and
// the resolved attempt; the phase comes from the bucket the message put it
// in, which is the only thing the scale set says about progress.
func (s *ScaleSetSource) event(ctx context.Context, message Message, job Job, phase demand.Phase) (demand.Event, error) {
	jobID, err := strconv.ParseInt(job.JobID, 10, 64)
	if err != nil || jobID <= 0 {
		return demand.Event{}, fmt.Errorf("%w: scale-set job id %q", demand.ErrInvalid, job.JobID)
	}
	meta, err := s.metadata.Resolve(ctx, job)
	if err != nil {
		return demand.Event{}, fmt.Errorf("resolve scale-set job %s metadata: %w", job.JobID, err)
	}
	if meta.JobID != job.JobID || meta.WorkflowRunID != job.WorkflowRunID {
		return demand.Event{}, fmt.Errorf("%w: metadata describes a different job", demand.ErrInvalid)
	}
	started := job.StartTime
	if started.IsZero() {
		started = job.AssignTime
	}
	event := demand.Event{
		Adapter:    demand.ScaleSetAdapter,
		DeliveryID: scaleSetDeliveryID(message, jobID, phase),
		Phase:      phase,
		JobID:      jobID,
		RunID:      job.WorkflowRunID,
		RunAttempt: meta.WorkflowAttempt,
		Owner:      job.Owner,
		Repository: job.Repository,
		Workflow:   job.WorkflowRef,
		JobName:    job.DisplayName,
		CommitSHA:  meta.CommitSHA,
		Labels:     append([]string(nil), job.Labels...),
		RunnerName: job.RunnerName,
		QueuedAt:   job.QueueTime,
		ObservedAt: s.now(),
	}
	if phase != demand.PhaseQueued {
		event.StartedAt = started
	}
	if phase == demand.PhaseCompleted {
		conclusion, ok := scaleSetConclusions[job.Result]
		if !ok {
			return demand.Event{}, fmt.Errorf("%w: scale-set result %q", demand.ErrInvalid, job.Result)
		}
		event.Conclusion = conclusion
		event.CompletedAt = job.FinishTime
	}
	if err := event.Validate(); err != nil {
		return demand.Event{}, fmt.Errorf("scale-set job %s: %w", job.JobID, err)
	}
	return event, nil
}

// scaleSetDeliveryID is evidence about this copy of the observation, stable
// per scale set, message and phase, so the same message replayed out of the
// inbox presents the same evidence and the store answers duplicate.
func scaleSetDeliveryID(message Message, jobID int64, phase demand.Phase) string {
	return "scaleset." + strconv.Itoa(message.ScaleSetID) + "." + strconv.Itoa(message.MessageID) +
		"." + strconv.FormatInt(jobID, 10) + "." + string(phase)
}
