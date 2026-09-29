// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// A delivery is the only account this plane gets of a job somewhere else, and
// every field of it is a stranger's. These hold the two ends of that: what a
// normalized delivery must state before it becomes demand, and what an
// endpoint answers when it cannot serve at all.

func aQueuedDelivery() string {
	return `{
		"action": "queued",
		"workflow_job": {
			"id": 42, "run_id": 7, "run_attempt": 1,
			"name": "build", "workflow_name": "ci",
			"head_sha": "` + strings.Repeat("a", 40) + `",
			"status": "queued",
			"labels": ["self-hosted", "ra8"],
			"created_at": "2026-09-28T12:00:00Z"
		},
		"repository": {
			"name": "ra8-firmware", "full_name": "bsikar/ra8-firmware",
			"owner": {"login": "bsikar"}
		}
	}`
}

func normalized(t *testing.T, payload string) (Event, error) {
	t.Helper()
	observed := time.Date(2026, 9, 28, 12, 0, 5, 0, time.UTC)
	return NormalizeWorkflowJob(WebhookAdapter, "d-1", []byte(payload), observed)
}

// The fixture first, so every refusal below is known to be about the field it
// spoils and not about the payload it is spoiled from.
func TestAQueuedDeliveryBecomesDemand(t *testing.T) {
	event, err := normalized(t, aQueuedDelivery())
	if err != nil {
		t.Fatalf("an ordinary queued delivery was refused: %v", err)
	}
	if event.Phase != PhaseQueued || event.JobID != 42 || event.RunID != 7 {
		t.Fatalf("event = %+v, want the queued job it describes", event)
	}
	if event.Owner != "bsikar" || event.Repository != "ra8-firmware" {
		t.Fatalf("event named %s/%s", event.Owner, event.Repository)
	}
	// The receipt time is the server's own, never the sender's.
	if !event.ObservedAt.Equal(time.Date(2026, 9, 28, 12, 0, 5, 0, time.UTC)) {
		t.Fatalf("observed at %v, want the server's receipt time", event.ObservedAt)
	}
	if !event.QueuedAt.Equal(time.Date(2026, 9, 28, 12, 0, 0, 0, time.UTC)) {
		t.Fatalf("queued at %v", event.QueuedAt)
	}
}

// A timestamp the payload states is parsed, and one it states badly is
// refused by name at each of the three places a delivery carries one.
func TestATimestampADeliveryStatesBadlyIsRefusedByName(t *testing.T) {
	for _, refused := range []struct {
		when    string
		payload string
	}{
		{"queued at", strings.Replace(aQueuedDelivery(), `"created_at": "2026-09-28T12:00:00Z"`, `"created_at": "yesterday"`, 1)},
		{"started at", strings.Replace(strings.Replace(aQueuedDelivery(),
			`"action": "queued"`, `"action": "in_progress"`, 1),
			`"status": "queued"`, `"status": "in_progress", "started_at": "soon"`, 1)},
		{"completed at", strings.Replace(strings.Replace(aQueuedDelivery(),
			`"action": "queued"`, `"action": "completed"`, 1),
			`"status": "queued"`, `"status": "completed", "started_at": "2026-09-28T12:00:01Z", "conclusion": "success", "completed_at": "never"`, 1)},
	} {
		t.Run(refused.when, func(t *testing.T) {
			_, err := normalized(t, refused.payload)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("error = %v, want ErrInvalid", err)
			}
			if !strings.Contains(err.Error(), "timestamp") {
				t.Fatalf("error = %v, want it to name the timestamp", err)
			}
		})
	}
}

// Validate is what keeps an adapter from emitting demand the rest of the
// plane cannot place. Each field is spoiled on its own, from a payload that
// is otherwise honest, and the refusal names that field.
func TestEveryFieldDemandNeedsIsNamedWhenItIsMissing(t *testing.T) {
	honest := func() Event {
		event, err := normalized(t, aQueuedDelivery())
		if err != nil {
			t.Fatal(err)
		}
		return event
	}

	for _, refused := range []struct {
		field string
		spoil func(*Event)
		says  string
	}{
		{"no adapter", func(e *Event) { e.Adapter = "" }, "adapter"},
		{"an adapter name too long", func(e *Event) { e.Adapter = strings.Repeat("a", 65) }, "adapter"},
		{"a delivery id that is not one", func(e *Event) { e.DeliveryID = "" }, "delivery id"},
		{"a phase no adapter emits", func(e *Event) { e.Phase = Phase("halfway") }, "phase"},
		{"no job id", func(e *Event) { e.JobID = 0 }, "job or run id"},
		{"a negative run id", func(e *Event) { e.RunID = -1 }, "job or run id"},
		{"a run attempt below one", func(e *Event) { e.RunAttempt = 0 }, "run attempt"},
		{"a run attempt past the thousandth", func(e *Event) { e.RunAttempt = 1001 }, "run attempt"},
		{"an owner that is not a login", func(e *Event) { e.Owner = "not a login" }, "repository"},
		{"no workflow", func(e *Event) { e.Workflow = "" }, "workflow or job name"},
		{"a job name too long", func(e *Event) { e.JobName = strings.Repeat("j", 256) }, "workflow or job name"},
		{"a commit that is not a sha", func(e *Event) { e.CommitSHA = "abc" }, "commit"},
		{"no labels", func(e *Event) { e.Labels = nil }, "labels"},
		{"a label repeated", func(e *Event) { e.Labels = []string{"ra8", "ra8"} }, "label"},
		{"no queue stamp", func(e *Event) { e.QueuedAt = time.Time{} }, "timestamps"},
		{"a runner name that is not one", func(e *Event) { e.RunnerName = "runner one" }, "runner name"},
	} {
		t.Run(refused.field, func(t *testing.T) {
			event := honest()
			refused.spoil(&event)
			err := event.Validate()
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("error = %v, want ErrInvalid", err)
			}
			if !strings.Contains(err.Error(), refused.says) {
				t.Fatalf("error = %v, want it to name %q", err, refused.says)
			}
		})
	}
}

// Supersede is how out-of-order delivery is survived, so it is judged on the
// unit of demand and the phase, never on arrival.
func TestALaterPhaseOfTheSameJobSupersedes(t *testing.T) {
	queued := honestEvent(t)
	running := honestEvent(t)
	running.Phase = PhaseInProgress

	if !running.Supersedes(queued) {
		t.Fatal("a job seen running does not supersede the same job seen queued")
	}
	if queued.Supersedes(running) {
		t.Fatal("a queued delivery superseded a later phase of the same job")
	}
	if queued.Supersedes(queued) {
		t.Fatal("the same phase delivered twice superseded itself")
	}

	other := honestEvent(t)
	other.JobID = 43
	if running.Supersedes(other) || other.Supersedes(running) {
		t.Fatal("a different unit of demand superseded across jobs")
	}
}

func honestEvent(t *testing.T) Event {
	t.Helper()
	event, err := normalized(t, aQueuedDelivery())
	if err != nil {
		t.Fatal(err)
	}
	return event
}

// An endpoint that came up wrong is refused at construction, not discovered
// at the first delivery.
func TestAnEndpointThatCameUpWrongIsRefusedAtConstruction(t *testing.T) {
	recorder := &countingRecorder{}
	for _, refused := range []struct {
		named  string
		config WebhookConfig
		says   string
	}{
		{"a secret too short", WebhookConfig{Secret: []byte("short"), Recorder: recorder}, "at least 16 bytes"},
		{"no recorder", WebhookConfig{Secret: []byte(strings.Repeat("s", 16))}, "event recorder"},
		{"an adapter name too long", WebhookConfig{Secret: []byte(strings.Repeat("s", 16)),
			Recorder: recorder, Adapter: strings.Repeat("a", 65)}, "adapter name is too long"},
	} {
		t.Run(refused.named, func(t *testing.T) {
			hook, err := NewWebhook(refused.config)
			if err == nil {
				t.Fatal("an endpoint that cannot serve was built anyway")
			}
			if !strings.Contains(err.Error(), refused.says) {
				t.Fatalf("error = %v, want it to name %q", err, refused.says)
			}
			if hook != nil {
				t.Fatal("a refused configuration returned an endpoint")
			}
		})
	}
}

// A webhook that is not there answers 500 rather than panicking on the
// delivery: a 5xx is the one answer that keeps the sender retrying, which is
// what an endpoint this broken deserves.
func TestAnEndpointThatIsNotThereAnswersARetryableFailure(t *testing.T) {
	var absent *Webhook
	recording := httptest.NewRecorder()
	absent.ServeHTTP(recording, httptest.NewRequest(http.MethodPost, "/webhook", strings.NewReader("{}")))

	if recording.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500 so the sender retries", recording.Code)
	}
	if !strings.Contains(recording.Body.String(), "webhook unavailable") {
		t.Fatalf("body = %q", recording.Body.String())
	}
}

type countingRecorder struct{ taken int }

func (c *countingRecorder) Record(ctx context.Context, event Event) error {
	c.taken++
	return nil
}
