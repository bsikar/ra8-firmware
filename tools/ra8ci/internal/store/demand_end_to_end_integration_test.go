//go:build integration

package store

import (
	"bytes"
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// These tests wire the real store to the real endpoint and the real
// reconciliation pass, and mock only GitHub: deliveries are signed the way a
// GitHub App signs them, and the jobs API is a stub. What they pin is the
// behaviour #1472 is actually about, which is that at-least-once delivery,
// out-of-order delivery and no delivery at all all end with the plane holding
// one correct unit of demand per job and run attempt.

var endToEndSecret = []byte("integration-webhook-secret")

type stubJobSource struct {
	snapshots map[int64]demand.JobSnapshot
	missing   map[int64]bool
	calls     int
}

func (s *stubJobSource) Job(_ context.Context, _, _ string, jobID int64) (demand.JobSnapshot, bool, error) {
	s.calls++
	if s.missing[jobID] {
		return demand.JobSnapshot{}, false, nil
	}
	snapshot, ok := s.snapshots[jobID]
	return snapshot, ok, nil
}

// endToEndPlane is the wiring under test: the endpoint writes through the
// adapter gate into the store, and the pass reads the same store back.
func endToEndPlane(t *testing.T) (*Store, http.Handler, *DemandSource) {
	t.Helper()
	store, pool := integrationStore(t)
	t.Cleanup(pool.Close)
	source := store.DemandSource()
	registry, err := demand.NewRegistry(source)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	webhook, err := demand.NewWebhook(demand.WebhookConfig{Secret: endToEndSecret, Recorder: registry})
	if err != nil {
		t.Fatalf("new webhook: %v", err)
	}
	return store, webhook, source
}

func workflowJobBody(jobID int64, attempt int, phase demand.Phase, queued time.Time) []byte {
	body := fmt.Sprintf(`{"action":%q,"workflow_job":{"id":%d,"run_id":%d,"run_attempt":%d,`+
		`"name":"build","workflow_name":"ci","head_sha":"0123456789abcdef0123456789abcdef01234567",`+
		`"status":%q,"labels":["self-hosted","ra8"],"runner_name":"ra8-lab-01","created_at":%q`,
		phase, jobID, jobID+1, attempt, phase, queued.UTC().Format(time.RFC3339))
	if phase != demand.PhaseQueued {
		body += fmt.Sprintf(`,"started_at":%q`, queued.Add(time.Minute).UTC().Format(time.RFC3339))
	}
	if phase == demand.PhaseCompleted {
		body += fmt.Sprintf(`,"conclusion":"success","completed_at":%q`,
			queued.Add(2*time.Minute).UTC().Format(time.RFC3339))
	}
	body += `},"repository":{"name":"ra8-firmware","full_name":"bsikar/ra8-firmware","owner":{"login":"bsikar"}}}`
	return []byte(body)
}

func deliver(t *testing.T, handler http.Handler, deliveryID string, body []byte, sign bool) int {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, "/webhook", bytes.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("X-GitHub-Event", "workflow_job")
	request.Header.Set("X-GitHub-Delivery", deliveryID)
	if sign {
		request.Header.Set("X-Hub-Signature-256", demand.SignDelivery(endToEndSecret, body))
	}
	recorder := httptest.NewRecorder()
	handler.ServeHTTP(recorder, request)
	return recorder.Code
}

func uniqueJobID() int64 { return time.Now().UnixNano() % 1000000000 }

// GitHub delivers at least once. Three copies of one delivery must leave one
// unit of demand that never moved.
func TestDemandEndToEndAbsorbsRetriedDeliveries(t *testing.T) {
	store, handler, _ := endToEndPlane(t)
	ctx := context.Background()
	jobID := uniqueJobID()
	body := workflowJobBody(jobID, 1, demand.PhaseQueued, time.Now().UTC())

	for i := 0; i < 3; i++ {
		if code := deliver(t, handler, "end-to-end-retry", body, true); code != http.StatusAccepted {
			t.Fatalf("delivery %d answered %d, want 202", i, code)
		}
	}
	record, err := store.GetDemandEvent(ctx, demand.JobKey(jobID, 1))
	if err != nil {
		t.Fatal(err)
	}
	if record.Version != 1 || record.Event.Phase != demand.PhaseQueued {
		t.Fatalf("retries moved the row: version %d phase %q", record.Version, record.Event.Phase)
	}
}

// Deliveries arrive out of order. A completion delivered first must not be
// undone by the queued event that follows it.
func TestDemandEndToEndKeepsTheLaterPhaseWhateverTheOrder(t *testing.T) {
	store, handler, _ := endToEndPlane(t)
	ctx := context.Background()
	jobID := uniqueJobID()
	queued := time.Now().UTC()

	if code := deliver(t, handler, "end-to-end-late-completed", workflowJobBody(jobID, 1, demand.PhaseCompleted, queued), true); code != http.StatusAccepted {
		t.Fatalf("completion answered %d, want 202", code)
	}
	if code := deliver(t, handler, "end-to-end-late-queued", workflowJobBody(jobID, 1, demand.PhaseQueued, queued), true); code != http.StatusAccepted {
		t.Fatalf("late queued answered %d, want 202", code)
	}
	record, err := store.GetDemandEvent(ctx, demand.JobKey(jobID, 1))
	if err != nil {
		t.Fatal(err)
	}
	if record.Event.Phase != demand.PhaseCompleted || record.Event.Conclusion != "success" {
		t.Fatalf("a late queued delivery undid the completion: %+v", record.Event)
	}
	open, err := store.ListOpenDemand(ctx, 100)
	if err != nil {
		t.Fatal(err)
	}
	for _, candidate := range open {
		if candidate.Event.Key() == demand.JobKey(jobID, 1) {
			t.Fatal("completed demand is still open")
		}
	}
}

// The completion is never delivered. The pass must turn it into a late run
// rather than leaving demand open forever.
func TestDemandEndToEndTurnsADroppedCompletionIntoALateRun(t *testing.T) {
	store, handler, source := endToEndPlane(t)
	ctx := context.Background()
	jobID := uniqueJobID()
	queued := time.Now().UTC().Add(-time.Hour)

	if code := deliver(t, handler, "end-to-end-dropped", workflowJobBody(jobID, 1, demand.PhaseQueued, queued), true); code != http.StatusAccepted {
		t.Fatalf("queued delivery answered %d, want 202", code)
	}
	jobs := &stubJobSource{snapshots: map[int64]demand.JobSnapshot{jobID: {
		Phase:       demand.PhaseCompleted,
		Conclusion:  "failure",
		RunnerName:  "ra8-lab-02",
		StartedAt:   queued.Add(time.Minute),
		CompletedAt: queued.Add(5 * time.Minute),
	}}}
	reconciler, err := demand.NewReconciler(demand.ReconcilerConfig{Source: jobs, Store: source,
		Grace: time.Minute, MissingAfter: 2 * time.Hour, BatchSize: 100,
		Now: func() time.Time { return time.Now().UTC() }})
	if err != nil {
		t.Fatalf("new reconciler: %v", err)
	}
	report, err := reconciler.Pass(ctx)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report.Advanced < 1 {
		t.Fatalf("pass advanced nothing: %+v", report)
	}
	record, err := store.GetDemandEvent(ctx, demand.JobKey(jobID, 1))
	if err != nil {
		t.Fatal(err)
	}
	if record.Event.Phase != demand.PhaseCompleted || record.Event.Conclusion != "failure" {
		t.Fatalf("the dropped completion was not reconciled: %+v", record.Event)
	}
	if record.Event.QueuedAt.IsZero() || record.Event.JobName != "build" {
		t.Fatalf("the pass lost the identity it merged onto: %+v", record.Event)
	}
}

// The forge no longer knows the job at all. Past MissingAfter the pass must
// conclude it so it stops being open, and the conclusion must say it was the
// pass that decided, not a real result.
func TestDemandEndToEndConcludesDemandTheForgeForgot(t *testing.T) {
	store, handler, source := endToEndPlane(t)
	ctx := context.Background()
	jobID := uniqueJobID()
	queued := time.Now().UTC().Add(-3 * time.Hour)

	if code := deliver(t, handler, "end-to-end-forgotten", workflowJobBody(jobID, 1, demand.PhaseQueued, queued), true); code != http.StatusAccepted {
		t.Fatalf("queued delivery answered %d, want 202", code)
	}
	jobs := &stubJobSource{missing: map[int64]bool{jobID: true}}
	reconciler, err := demand.NewReconciler(demand.ReconcilerConfig{Source: jobs, Store: source,
		Grace: time.Minute, MissingAfter: time.Hour, BatchSize: 100,
		Now: func() time.Time { return time.Now().UTC() }})
	if err != nil {
		t.Fatalf("new reconciler: %v", err)
	}
	if _, err := reconciler.Pass(ctx); err != nil {
		t.Fatalf("pass: %v", err)
	}
	record, err := store.GetDemandEvent(ctx, demand.JobKey(jobID, 1))
	if err != nil {
		t.Fatal(err)
	}
	if record.Event.Phase != demand.PhaseCompleted || record.Event.Conclusion != "stale" {
		t.Fatalf("forgotten demand stayed open: %+v", record.Event)
	}
}

// Fresh demand is left alone: a delivery in flight is the common case, and
// polling inside the grace window spends API budget racing the webhook.
func TestDemandEndToEndLeavesFreshDemandAlone(t *testing.T) {
	_, handler, source := endToEndPlane(t)
	ctx := context.Background()
	jobID := uniqueJobID()

	if code := deliver(t, handler, "end-to-end-fresh", workflowJobBody(jobID, 1, demand.PhaseQueued, time.Now().UTC()), true); code != http.StatusAccepted {
		t.Fatalf("queued delivery answered %d, want 202", code)
	}
	jobs := &stubJobSource{snapshots: map[int64]demand.JobSnapshot{}}
	reconciler, err := demand.NewReconciler(demand.ReconcilerConfig{Source: jobs, Store: source,
		Grace: time.Hour, MissingAfter: 2 * time.Hour, BatchSize: 100,
		Now: func() time.Time { return time.Now().UTC() }})
	if err != nil {
		t.Fatalf("new reconciler: %v", err)
	}
	report, err := reconciler.Pass(ctx)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report.Waiting < 1 {
		t.Fatalf("fresh demand was not left waiting: %+v", report)
	}
	if jobs.calls != 0 {
		t.Fatalf("the pass called the forge %d times inside the grace window", jobs.calls)
	}
}

// One job retried by GitHub is a new run attempt and therefore new demand.
func TestDemandEndToEndKeepsRunAttemptsApart(t *testing.T) {
	store, handler, _ := endToEndPlane(t)
	ctx := context.Background()
	jobID := uniqueJobID()
	queued := time.Now().UTC()

	if code := deliver(t, handler, "end-to-end-attempt-1", workflowJobBody(jobID, 1, demand.PhaseCompleted, queued), true); code != http.StatusAccepted {
		t.Fatalf("attempt 1 answered %d, want 202", code)
	}
	if code := deliver(t, handler, "end-to-end-attempt-2", workflowJobBody(jobID, 2, demand.PhaseQueued, queued), true); code != http.StatusAccepted {
		t.Fatalf("attempt 2 answered %d, want 202", code)
	}
	first, err := store.GetDemandEvent(ctx, demand.JobKey(jobID, 1))
	if err != nil {
		t.Fatal(err)
	}
	second, err := store.GetDemandEvent(ctx, demand.JobKey(jobID, 2))
	if err != nil {
		t.Fatal(err)
	}
	if first.Event.Phase != demand.PhaseCompleted || second.Event.Phase != demand.PhaseQueued {
		t.Fatalf("attempts bled into each other: %q then %q", first.Event.Phase, second.Event.Phase)
	}
}

// An unsigned delivery is rejected before it is parsed, and nothing about it
// reaches the store.
func TestDemandEndToEndRefusesAnUnsignedDelivery(t *testing.T) {
	store, handler, _ := endToEndPlane(t)
	ctx := context.Background()
	jobID := uniqueJobID()

	if code := deliver(t, handler, "end-to-end-unsigned", workflowJobBody(jobID, 1, demand.PhaseQueued, time.Now().UTC()), false); code != http.StatusUnauthorized {
		t.Fatalf("unsigned delivery answered %d, want 401", code)
	}
	if _, err := store.GetDemandEvent(ctx, demand.JobKey(jobID, 1)); err == nil {
		t.Fatal("an unsigned delivery reached the store")
	}
}
