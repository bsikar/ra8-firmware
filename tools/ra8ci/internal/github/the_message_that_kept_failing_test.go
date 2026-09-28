// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// One scale-set message can carry a whole batch of jobs. A message the forge
// cannot answer for would otherwise have this source spend a metadata lookup
// per job on the way to failing every one of them, and a job recorded with a
// piece missing would sit in the demand store as evidence nobody can use.

// A message that keeps failing is abandoned at the bound rather than carried
// to its end, and the first failure is the one reported.
func TestAMessageThatKeepsFailingIsAbandonedAtTheBound(t *testing.T) {
	metadata := &metadataStub{meta: scaleSetMetadata()}
	source, recorder := enabledScaleSetSource(t, metadata)

	message := Message{ScaleSetID: 42, MessageID: 7}
	for index := 0; index < maxScaleSetDemandFailures+4; index++ {
		job := scaleSetJob()
		job.JobID = "job-" + strconv.Itoa(index)
		message.Available = append(message.Available, job)
	}

	report, err := source.Observe(context.Background(), message)
	if err == nil || !errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("a message of unusable jobs answered %v", err)
	}
	if !strings.Contains(err.Error(), `"job-0"`) {
		t.Fatalf("the reported failure is %v, want the first job's", err)
	}
	if report.Scanned != maxScaleSetDemandFailures || report.Failed != maxScaleSetDemandFailures {
		t.Fatalf("the message was scanned %d and failed %d, want both at the bound of %d",
			report.Scanned, report.Failed, maxScaleSetDemandFailures)
	}
	if report.Recorded != 0 || len(recorder.events) != 0 {
		t.Fatalf("an abandoned message recorded %d events", len(recorder.events))
	}
	if metadata.calls != 0 {
		t.Fatalf("the forge was asked %d times about jobs with no usable id", metadata.calls)
	}
}

// A job whose metadata the forge would not resolve is counted and named, and
// the rest of the message is still read: one unresolvable job is not a
// message-wide failure.
func TestAJobWhoseMetadataWillNotResolveIsCountedAndNamed(t *testing.T) {
	metadata := &metadataStub{err: errors.New("workflow run is gone")}
	source, recorder := enabledScaleSetSource(t, metadata)

	report, err := source.Observe(context.Background(), Message{
		ScaleSetID: 42, MessageID: 7, Available: []Job{scaleSetJob()},
	})
	if err == nil || !strings.Contains(err.Error(), "resolve scale-set job 884422 metadata") ||
		!strings.Contains(err.Error(), "workflow run is gone") {
		t.Fatalf("an unresolvable job answered %v", err)
	}
	if report.Scanned != 1 || report.Failed != 1 || report.Recorded != 0 {
		t.Fatalf("an unresolvable job answered %+v", report)
	}
	if len(recorder.events) != 0 {
		t.Fatalf("an unresolvable job recorded %d events", len(recorder.events))
	}
	if metadata.calls != 1 {
		t.Fatalf("the forge was asked %d times about one job", metadata.calls)
	}
}

// A job the scale set started without a start time carries the moment it was
// assigned instead, so a started job is never recorded as having begun at the
// zero time.
func TestAStartedJobWithNoStartTimeCarriesItsAssignTime(t *testing.T) {
	source, recorder := enabledScaleSetSource(t, &metadataStub{meta: scaleSetMetadata()})

	job := scaleSetJob()
	job.StartTime = time.Time{}
	report, err := source.Observe(context.Background(), Message{
		ScaleSetID: 42, MessageID: 7, Started: []Job{job},
	})
	if err != nil {
		t.Fatalf("a job with no start time answered %v", err)
	}
	if report.Recorded != 1 || len(recorder.events) != 1 {
		t.Fatalf("a job with no start time answered %+v", report)
	}
	recorded := recorder.events[0]
	if !recorded.StartedAt.Equal(job.AssignTime) {
		t.Fatalf("a job with no start time began at %v, want its assign time %v", recorded.StartedAt, job.AssignTime)
	}
	if recorded.StartedAt.IsZero() {
		t.Fatal("a started job was recorded as beginning at the zero time")
	}
}

// An event assembled with a piece missing is refused rather than recorded, so
// the demand store never holds a row a later reader cannot act on.
func TestAnEventMissingAPieceIsRefusedRatherThanRecorded(t *testing.T) {
	for _, broken := range []struct {
		name string
		make func() Job
	}{
		{"no owner", func() Job { job := scaleSetJob(); job.Owner = ""; return job }},
		{"no repository", func() Job { job := scaleSetJob(); job.Repository = ""; return job }},
		{"no job name", func() Job { job := scaleSetJob(); job.DisplayName = ""; return job }},
		{"no workflow reference", func() Job { job := scaleSetJob(); job.WorkflowRef = ""; return job }},
	} {
		source, recorder := enabledScaleSetSource(t, &metadataStub{meta: scaleSetMetadata()})
		report, err := source.Observe(context.Background(), Message{
			ScaleSetID: 42, MessageID: 7, Available: []Job{broken.make()},
		})
		if err == nil {
			t.Errorf("a job with %s was recorded without complaint", broken.name)
			continue
		}
		if !strings.Contains(err.Error(), "scale-set job 884422") {
			t.Errorf("a job with %s answered %v, want the job named", broken.name, err)
		}
		if report.Failed != 1 || report.Recorded != 0 || len(recorder.events) != 0 {
			t.Errorf("a job with %s answered %+v and recorded %d events", broken.name, report, len(recorder.events))
		}
	}
}
