// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

type demandRecorderStub struct {
	events []demand.Event
	err    error
}

func (r *demandRecorderStub) Record(_ context.Context, event demand.Event) error {
	if r.err != nil {
		return r.err
	}
	r.events = append(r.events, event)
	return nil
}

type metadataStub struct {
	meta  JobMetadata
	err   error
	calls int
}

func (m *metadataStub) Resolve(_ context.Context, job Job) (JobMetadata, error) {
	m.calls++
	if m.err != nil {
		return JobMetadata{}, m.err
	}
	meta := m.meta
	if meta.JobID == "" {
		meta.JobID = job.JobID
	}
	if meta.WorkflowRunID == 0 {
		meta.WorkflowRunID = job.WorkflowRunID
	}
	return meta, nil
}

var scaleSetQueuedAt = time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)

func scaleSetJob() Job {
	return Job{
		RunnerRequestID: 5,
		Repository:      "ra8-firmware",
		Owner:           "bsikar",
		JobID:           "884422",
		WorkflowRef:     "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/main",
		DisplayName:     "build",
		WorkflowRunID:   77,
		EventName:       "push",
		Labels:          []string{"ra8-lab"},
		QueueTime:       scaleSetQueuedAt,
		AssignTime:      scaleSetQueuedAt.Add(time.Minute),
		StartTime:       scaleSetQueuedAt.Add(2 * time.Minute),
		FinishTime:      scaleSetQueuedAt.Add(9 * time.Minute),
	}
}

func scaleSetMetadata() JobMetadata {
	return JobMetadata{
		WorkflowAttempt: 2,
		CommitSHA:       "0123456789abcdef0123456789abcdef01234567",
		JobID:           "884422",
		WorkflowRunID:   77,
		Repository:      "bsikar/ra8-firmware",
	}
}

func enabledScaleSetSource(t *testing.T, metadata MetadataSource) (*ScaleSetSource, *demandRecorderStub) {
	t.Helper()
	recorder := &demandRecorderStub{}
	registry, err := demand.NewRegistry(recorder, demand.ScaleSetAdapter)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	source, err := NewScaleSetSource(registry, metadata)
	if err != nil {
		t.Fatalf("new scale-set source: %v", err)
	}
	source.now = func() time.Time { return scaleSetQueuedAt.Add(10 * time.Minute) }
	return source, recorder
}

func TestScaleSetSourceIsOffUntilTheAdapterIsNamed(t *testing.T) {
	recorder := &demandRecorderStub{}
	registry, err := demand.NewRegistry(recorder)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	metadata := &metadataStub{meta: scaleSetMetadata()}
	source, err := NewScaleSetSource(registry, metadata)
	if err != nil {
		t.Fatalf("new scale-set source: %v", err)
	}
	if source.Enabled() {
		t.Fatal("the scale-set source must be off by default")
	}
	report, err := source.Observe(context.Background(), Message{ScaleSetID: 1, MessageID: 4, Available: []Job{scaleSetJob()}})
	if err != nil {
		t.Fatalf("observe while disabled: %v", err)
	}
	if report.Scanned != 0 || report.Recorded != 0 || report.Failed != 0 {
		t.Fatalf("a disabled source did work: %+v", report)
	}
	if metadata.calls != 0 {
		t.Fatal("a disabled source spent a metadata lookup")
	}
	if len(recorder.events) != 0 {
		t.Fatal("a disabled source wrote demand")
	}
}

func TestScaleSetSourceNormalizesEveryBucket(t *testing.T) {
	source, recorder := enabledScaleSetSource(t, &metadataStub{meta: scaleSetMetadata()})
	completed := scaleSetJob()
	completed.Result = "succeeded"
	completed.RunnerName = "ra8-lab-01"
	message := Message{
		ScaleSetID: 3,
		MessageID:  91,
		Available:  []Job{scaleSetJob()},
		Started:    []Job{scaleSetJob()},
		Completed:  []Job{completed},
	}
	report, err := source.Observe(context.Background(), message)
	if err != nil {
		t.Fatalf("observe: %v", err)
	}
	if report.Scanned != 3 || report.Recorded != 3 || report.Failed != 0 {
		t.Fatalf("report = %+v, want 3 scanned and 3 recorded", report)
	}
	wantPhases := []demand.Phase{demand.PhaseQueued, demand.PhaseInProgress, demand.PhaseCompleted}
	for i, event := range recorder.events {
		if event.Phase != wantPhases[i] {
			t.Fatalf("event %d phase = %q, want %q", i, event.Phase, wantPhases[i])
		}
		if event.Adapter != demand.ScaleSetAdapter {
			t.Fatalf("event %d adapter = %q", i, event.Adapter)
		}
		if event.Key() != "884422/2" {
			t.Fatalf("event %d key = %q, want the job and the resolved attempt", i, event.Key())
		}
		if err := event.Validate(); err != nil {
			t.Fatalf("event %d does not satisfy the contract: %v", i, err)
		}
	}
	if recorder.events[0].DeliveryID == recorder.events[1].DeliveryID {
		t.Fatal("two phases of one job shared a delivery id")
	}
	if got := recorder.events[2].Conclusion; got != "success" {
		t.Fatalf("conclusion = %q, want success", got)
	}
	if recorder.events[0].StartedAt != (time.Time{}) {
		t.Fatal("queued demand must not claim a start time")
	}
}

func TestScaleSetDeliveryIDIsStablePerMessageAndPhase(t *testing.T) {
	first := scaleSetDeliveryID(Message{ScaleSetID: 3, MessageID: 91}, 884422, demand.PhaseQueued)
	again := scaleSetDeliveryID(Message{ScaleSetID: 3, MessageID: 91}, 884422, demand.PhaseQueued)
	if first != again {
		t.Fatalf("delivery id is not stable: %q then %q", first, again)
	}
	if later := scaleSetDeliveryID(Message{ScaleSetID: 3, MessageID: 92}, 884422, demand.PhaseQueued); later == first {
		t.Fatal("a later message reused the delivery id")
	}
	if other := scaleSetDeliveryID(Message{ScaleSetID: 3, MessageID: 91}, 884422, demand.PhaseCompleted); other == first {
		t.Fatal("a different phase reused the delivery id")
	}
}

func TestScaleSetSourceRefusesMetadataForAnotherJob(t *testing.T) {
	meta := scaleSetMetadata()
	meta.WorkflowRunID = 78
	source, recorder := enabledScaleSetSource(t, &metadataStub{meta: meta})
	report, err := source.Observe(context.Background(), Message{ScaleSetID: 1, MessageID: 2, Available: []Job{scaleSetJob()}})
	if !errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("observe = %v, want ErrInvalid", err)
	}
	if report.Failed != 1 || report.Recorded != 0 || len(recorder.events) != 0 {
		t.Fatalf("mismatched metadata was recorded: %+v", report)
	}
}

func TestScaleSetSourceRefusesAnUnknownResult(t *testing.T) {
	source, recorder := enabledScaleSetSource(t, &metadataStub{meta: scaleSetMetadata()})
	job := scaleSetJob()
	job.Result = "exploded"
	report, err := source.Observe(context.Background(), Message{ScaleSetID: 1, MessageID: 2, Completed: []Job{job}})
	if !errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("observe = %v, want ErrInvalid", err)
	}
	if report.Failed != 1 || len(recorder.events) != 0 {
		t.Fatal("an unmappable result became a conclusion")
	}
}

func TestScaleSetSourceCountsOneBadJobAndKeepsGoing(t *testing.T) {
	source, recorder := enabledScaleSetSource(t, &metadataStub{meta: scaleSetMetadata()})
	bad := scaleSetJob()
	bad.JobID = "not-a-number"
	report, err := source.Observe(context.Background(), Message{ScaleSetID: 1, MessageID: 2, Available: []Job{bad, scaleSetJob()}})
	if !errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("observe = %v, want the per-job failure", err)
	}
	if report.Scanned != 2 || report.Recorded != 1 || report.Failed != 1 {
		t.Fatalf("report = %+v, want one recorded and one failed", report)
	}
	if len(recorder.events) != 1 {
		t.Fatalf("recorded %d events, want 1", len(recorder.events))
	}
}

func TestScaleSetSourceStopsOnAStoreFailure(t *testing.T) {
	sentinel := errors.New("store down")
	recorder := &demandRecorderStub{err: sentinel}
	registry, err := demand.NewRegistry(recorder, demand.ScaleSetAdapter)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	source, err := NewScaleSetSource(registry, &metadataStub{meta: scaleSetMetadata()})
	if err != nil {
		t.Fatalf("new scale-set source: %v", err)
	}
	report, err := source.Observe(context.Background(), Message{ScaleSetID: 1, MessageID: 2, Available: []Job{scaleSetJob(), scaleSetJob()}})
	if !errors.Is(err, sentinel) {
		t.Fatalf("observe = %v, want the store failure", err)
	}
	if report.Scanned != 1 || report.Recorded != 0 {
		t.Fatalf("report = %+v, want the pass to stop at the first write failure", report)
	}
}

func TestScaleSetSourceRefusesMissingDependencies(t *testing.T) {
	if _, err := NewScaleSetSource(nil, &metadataStub{}); err == nil {
		t.Fatal("a source without a registry must be refused")
	}
	registry, err := demand.NewRegistry(&demandRecorderStub{}, demand.ScaleSetAdapter)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	if _, err := NewScaleSetSource(registry, nil); err == nil {
		t.Fatal("a source without a metadata resolver must be refused")
	}
	var nilSource *ScaleSetSource
	if nilSource.Enabled() {
		t.Fatal("a nil source must not claim to be enabled")
	}
}
