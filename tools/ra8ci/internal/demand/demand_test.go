// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"
)

var observedAt = time.Date(2026, 9, 24, 17, 0, 0, 0, time.UTC)

type deliveryOptions struct {
	action      string
	status      string
	jobID       int64
	runID       int64
	runAttempt  int
	fullName    string
	labels      []string
	conclusion  *string
	completedAt *string
	startedAt   string
	headSHA     string
}

func baseDelivery() deliveryOptions {
	return deliveryOptions{action: "queued", status: "queued", jobID: 44021, runID: 9001,
		runAttempt: 1, fullName: "bsikar/ra8-firmware", labels: []string{"self-hosted", "ra8-lab"},
		startedAt: "2026-09-24T16:58:00Z", headSHA: strings.Repeat("a", 40)}
}

func payloadFor(t *testing.T, options deliveryOptions) []byte {
	t.Helper()
	job := map[string]any{"id": options.jobID, "run_id": options.runID,
		"run_attempt": options.runAttempt, "name": "gates", "workflow_name": "ci",
		"head_sha": options.headSHA, "status": options.status, "labels": options.labels,
		"runner_name": "ra8-lab-01", "created_at": "2026-09-24T16:57:00Z",
		"started_at": options.startedAt, "conclusion": options.conclusion,
		"completed_at": options.completedAt}
	body := map[string]any{"action": options.action, "workflow_job": job,
		"repository": map[string]any{"name": "ra8-firmware", "full_name": options.fullName,
			"owner": map[string]any{"login": "bsikar"}}}
	raw, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestNormalizeQueuedDelivery(t *testing.T) {
	event, err := NormalizeWorkflowJob("github-app", "d1", payloadFor(t, baseDelivery()), observedAt)
	if err != nil {
		t.Fatal(err)
	}
	if event.Key() != "44021/1" {
		t.Fatalf("key is %q", event.Key())
	}
	if event.FullRepository() != "bsikar/ra8-firmware" || event.Phase != PhaseQueued {
		t.Fatalf("unexpected event: %+v", event)
	}
	if !event.ObservedAt.Equal(observedAt) {
		t.Fatalf("observation time came from the payload: %s", event.ObservedAt)
	}
	if !event.StartedAt.IsZero() || event.Conclusion != "" {
		t.Fatalf("queued event carries execution state: %+v", event)
	}
	if !event.QueuedAt.Equal(time.Date(2026, 9, 24, 16, 57, 0, 0, time.UTC)) {
		t.Fatalf("queue time is %s", event.QueuedAt)
	}
}

// A redelivery is the same unit of demand however many delivery ids GitHub
// spends on it: the identity is the job and the attempt, and the delivery id
// is evidence about the copy, not a key.
func TestDuplicateDeliveriesShareOneKey(t *testing.T) {
	first, err := NormalizeWorkflowJob("github-app", "d1", payloadFor(t, baseDelivery()), observedAt)
	if err != nil {
		t.Fatal(err)
	}
	second, err := NormalizeWorkflowJob("github-app", "d2", payloadFor(t, baseDelivery()),
		observedAt.Add(time.Minute))
	if err != nil {
		t.Fatal(err)
	}
	if first.Key() != second.Key() {
		t.Fatalf("redelivery changed the key: %q vs %q", first.Key(), second.Key())
	}
	if first.DeliveryID == second.DeliveryID {
		t.Fatal("the test did not actually vary the delivery id")
	}
	if second.Supersedes(first) || first.Supersedes(second) {
		t.Fatal("a duplicate delivery superseded its own first copy")
	}
}

// A retried workflow run is new demand, not a duplicate of the old one.
func TestRetriedRunAttemptIsDistinctDemand(t *testing.T) {
	first, err := NormalizeWorkflowJob("github-app", "d1", payloadFor(t, baseDelivery()), observedAt)
	if err != nil {
		t.Fatal(err)
	}
	retried := baseDelivery()
	retried.runAttempt = 2
	second, err := NormalizeWorkflowJob("github-app", "d2", payloadFor(t, retried), observedAt)
	if err != nil {
		t.Fatal(err)
	}
	if first.Key() == second.Key() {
		t.Fatalf("attempt 2 collided with attempt 1 on %q", first.Key())
	}
	if second.Supersedes(first) {
		t.Fatal("a different unit of demand superseded an earlier one")
	}
}

// Out-of-order deliveries: the later phase wins whichever arrives second.
func TestLaterPhaseSupersedesRegardlessOfArrivalOrder(t *testing.T) {
	queued, err := NormalizeWorkflowJob("github-app", "d1", payloadFor(t, baseDelivery()), observedAt)
	if err != nil {
		t.Fatal(err)
	}
	runningOptions := baseDelivery()
	runningOptions.action, runningOptions.status = "in_progress", "in_progress"
	running, err := NormalizeWorkflowJob("github-app", "d2", payloadFor(t, runningOptions), observedAt)
	if err != nil {
		t.Fatal(err)
	}
	success, completed := "success", "2026-09-24T17:05:00Z"
	doneOptions := baseDelivery()
	doneOptions.action, doneOptions.status = "completed", "completed"
	doneOptions.conclusion, doneOptions.completedAt = &success, &completed
	done, err := NormalizeWorkflowJob("github-app", "d3", payloadFor(t, doneOptions), observedAt)
	if err != nil {
		t.Fatal(err)
	}
	for _, pair := range []struct {
		later, earlier Event
	}{{running, queued}, {done, queued}, {done, running}} {
		if !pair.later.Supersedes(pair.earlier) {
			t.Fatalf("%q did not supersede %q", pair.later.Phase, pair.earlier.Phase)
		}
		if pair.earlier.Supersedes(pair.later) {
			t.Fatalf("%q superseded %q", pair.earlier.Phase, pair.later.Phase)
		}
	}
	if done.Conclusion != "success" || done.CompletedAt.IsZero() || done.StartedAt.IsZero() {
		t.Fatalf("completed event lost its outcome: %+v", done)
	}
}

func TestIgnoredAndInvalidDeliveries(t *testing.T) {
	success := "success"
	completed := "2026-09-24T17:05:00Z"
	early := "2026-09-24T16:00:00Z"
	mutate := func(apply func(*deliveryOptions)) []byte {
		options := baseDelivery()
		apply(&options)
		return payloadFor(t, options)
	}
	ignored := mutate(func(o *deliveryOptions) { o.action, o.status = "waiting", "waiting" })
	if _, err := NormalizeWorkflowJob("github-app", "d1", ignored, observedAt); !errors.Is(err, ErrIgnored) {
		t.Fatalf("waiting action returned %v, want ErrIgnored", err)
	}
	cases := map[string][]byte{
		"status disagrees with action": mutate(func(o *deliveryOptions) { o.status = "in_progress" }),
		"missing job id":               mutate(func(o *deliveryOptions) { o.jobID = 0 }),
		"zero run attempt":             mutate(func(o *deliveryOptions) { o.runAttempt = 0 }),
		"repository disagrees":         mutate(func(o *deliveryOptions) { o.fullName = "someone/else" }),
		"short commit":                 mutate(func(o *deliveryOptions) { o.headSHA = "abc" }),
		"no labels":                    mutate(func(o *deliveryOptions) { o.labels = nil }),
		"duplicate labels":             mutate(func(o *deliveryOptions) { o.labels = []string{"ra8-lab", "ra8-lab"} }),
		"conclusion before completion": mutate(func(o *deliveryOptions) { o.conclusion = &success }),
		"completed before queued": mutate(func(o *deliveryOptions) {
			o.action, o.status = "completed", "completed"
			o.conclusion, o.completedAt = &success, &early
		}),
		"completed without conclusion": mutate(func(o *deliveryOptions) {
			o.action, o.status = "completed", "completed"
			o.completedAt = &completed
		}),
		"not json": []byte("{"),
		"empty":    nil,
	}
	for name, payload := range cases {
		if _, err := NormalizeWorkflowJob("github-app", "d1", payload, observedAt); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s returned %v, want ErrInvalid", name, err)
		}
	}
	if _, err := NormalizeWorkflowJob("github-app", "not a delivery id!", payloadFor(t, baseDelivery()),
		observedAt); !errors.Is(err, ErrInvalid) {
		t.Fatalf("bad delivery id returned %v, want ErrInvalid", err)
	}
	oversized := make([]byte, MaxPayloadBytes+1)
	if _, err := NormalizeWorkflowJob("github-app", "d1", oversized, observedAt); !errors.Is(err, ErrInvalid) {
		t.Fatalf("oversized payload returned %v, want ErrInvalid", err)
	}
}

// Rank is what the out-of-order rule is built on, so it is pinned directly:
// every phase this plane acts on ranks above the zero value, and the order is
// the lifecycle order.
func TestPhaseRankOrdersTheLifecycle(t *testing.T) {
	if Phase("").Rank() != 0 || Phase("waiting").Rank() != 0 {
		t.Fatal("an unhandled phase ranks as a real one")
	}
	if !(PhaseQueued.Rank() < PhaseInProgress.Rank() && PhaseInProgress.Rank() < PhaseCompleted.Rank()) {
		t.Fatal("phase ranks are not in lifecycle order")
	}
}
