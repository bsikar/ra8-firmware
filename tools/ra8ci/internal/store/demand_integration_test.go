//go:build integration

package store

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

func demandFixture(t *testing.T, jobID int64, attempt int, phase demand.Phase, deliveryID string) demand.Event {
	t.Helper()
	queued := time.Now().UTC().Truncate(time.Millisecond)
	event := demand.Event{Adapter: "github-app", DeliveryID: deliveryID, Phase: phase,
		JobID: jobID, RunID: jobID + 1, RunAttempt: attempt, Owner: "bsikar",
		Repository: "ra8-firmware", Workflow: "ci", JobName: "build",
		CommitSHA: "0123456789abcdef0123456789abcdef01234567",
		Labels:    []string{"self-hosted", "ra8"}, QueuedAt: queued, ObservedAt: queued}
	if phase != demand.PhaseQueued {
		event.StartedAt = queued.Add(time.Minute)
		event.RunnerName = "ra8-lab-01"
	}
	if phase == demand.PhaseCompleted {
		event.Conclusion = "success"
		event.CompletedAt = queued.Add(2 * time.Minute)
	}
	return event
}

// A delivery GitHub retries must not be counted twice, and the row must not
// move a second time on the replay.
func TestRecordDemandEventDeduplicatesDeliveries(t *testing.T) {
	store, pool := integrationStore(t)
	defer pool.Close()
	ctx := context.Background()
	jobID := time.Now().UnixNano() % 1000000000
	event := demandFixture(t, jobID, 1, demand.PhaseQueued, "delivery-a")

	outcome, err := store.RecordDemandEvent(ctx, event)
	if err != nil || outcome != DemandAccepted {
		t.Fatalf("first delivery: %s, %v", outcome, err)
	}
	for i := 0; i < 3; i++ {
		outcome, err = store.RecordDemandEvent(ctx, event)
		if err != nil || outcome != DemandDuplicate {
			t.Fatalf("replay %d: %s, %v", i, outcome, err)
		}
	}
	record, err := store.GetDemandEvent(ctx, event.Key())
	if err != nil {
		t.Fatal(err)
	}
	if record.Version != 1 {
		t.Fatalf("replays moved the row: version %d", record.Version)
	}
	if record.Event.Phase != demand.PhaseQueued || len(record.Event.Labels) != 2 {
		t.Fatalf("stored event came back wrong: %+v", record.Event)
	}
}

// Deliveries arrive out of order. The later phase has to win whichever one
// is delivered second, and the earlier one must not undo it.
func TestRecordDemandEventOutOfOrderDeliveries(t *testing.T) {
	store, pool := integrationStore(t)
	defer pool.Close()
	ctx := context.Background()
	jobID := time.Now().UnixNano()%1000000000 + 1

	completed := demandFixture(t, jobID, 1, demand.PhaseCompleted, "delivery-completed")
	if outcome, err := store.RecordDemandEvent(ctx, completed); err != nil || outcome != DemandAccepted {
		t.Fatalf("completed first: %s, %v", outcome, err)
	}
	queued := demandFixture(t, jobID, 1, demand.PhaseQueued, "delivery-queued")
	if outcome, err := store.RecordDemandEvent(ctx, queued); err != nil || outcome != DemandStale {
		t.Fatalf("late queued: %s, %v", outcome, err)
	}
	record, err := store.GetDemandEvent(ctx, completed.Key())
	if err != nil {
		t.Fatal(err)
	}
	if record.Event.Phase != demand.PhaseCompleted || record.Event.Conclusion != "success" {
		t.Fatalf("a late queued delivery undid the completion: %+v", record.Event)
	}
	if record.Version != 1 {
		t.Fatalf("stale delivery moved the row: version %d", record.Version)
	}
	// Both deliveries are still on file even though only one moved the row.
	var deliveries int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM demand_deliveries WHERE demand_key=$1`,
		completed.Key()).Scan(&deliveries); err != nil {
		t.Fatal(err)
	}
	if deliveries != 2 {
		t.Fatalf("got %d deliveries on file, want 2", deliveries)
	}
}

// The ordinary path: queued, then in_progress, then completed, each moving
// the one row rather than making another.
func TestRecordDemandEventAdvancesOneRow(t *testing.T) {
	store, pool := integrationStore(t)
	defer pool.Close()
	ctx := context.Background()
	jobID := time.Now().UnixNano()%1000000000 + 2

	queued := demandFixture(t, jobID, 1, demand.PhaseQueued, "delivery-q")
	if _, err := store.RecordDemandEvent(ctx, queued); err != nil {
		t.Fatal(err)
	}
	open, err := store.ListOpenDemand(ctx, 100)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, record := range open {
		if record.Event.Key() == queued.Key() {
			found = true
		}
	}
	if !found {
		t.Fatal("queued demand is not open demand")
	}
	for _, step := range []struct {
		phase    demand.Phase
		delivery string
	}{{demand.PhaseInProgress, "delivery-p"}, {demand.PhaseCompleted, "delivery-c"}} {
		outcome, err := store.RecordDemandEvent(ctx, demandFixture(t, jobID, 1, step.phase, step.delivery))
		if err != nil || outcome != DemandSuperseded {
			t.Fatalf("%s: %s, %v", step.phase, outcome, err)
		}
	}
	record, err := store.GetDemandEvent(ctx, queued.Key())
	if err != nil {
		t.Fatal(err)
	}
	if record.Event.Phase != demand.PhaseCompleted || record.Version != 3 {
		t.Fatalf("got %s at version %d, want completed at 3", record.Event.Phase, record.Version)
	}
	if record.Event.RunnerName != "ra8-lab-01" || record.Event.CompletedAt.IsZero() {
		t.Fatalf("later evidence was dropped: %+v", record.Event)
	}
	if !record.UpdatedAt.After(record.FirstSeenAt) && record.UpdatedAt.Equal(record.FirstSeenAt) {
		t.Fatal("updated_at did not move with the row")
	}
	open, err = store.ListOpenDemand(ctx, 100)
	if err != nil {
		t.Fatal(err)
	}
	for _, candidate := range open {
		if candidate.Event.Key() == queued.Key() {
			t.Fatal("completed demand is still listed as open")
		}
	}
}

// A retried workflow run is new demand, not a later phase of the old one.
func TestRecordDemandEventSeparatesRunAttempts(t *testing.T) {
	store, pool := integrationStore(t)
	defer pool.Close()
	ctx := context.Background()
	jobID := time.Now().UnixNano()%1000000000 + 3

	first := demandFixture(t, jobID, 1, demand.PhaseCompleted, "delivery-first")
	if _, err := store.RecordDemandEvent(ctx, first); err != nil {
		t.Fatal(err)
	}
	retry := demandFixture(t, jobID, 2, demand.PhaseQueued, "delivery-retry")
	outcome, err := store.RecordDemandEvent(ctx, retry)
	if err != nil || outcome != DemandAccepted {
		t.Fatalf("retry attempt: %s, %v", outcome, err)
	}
	retried, err := store.GetDemandEvent(ctx, retry.Key())
	if err != nil {
		t.Fatal(err)
	}
	if retried.Event.Phase != demand.PhaseQueued || retried.Event.RunAttempt != 2 {
		t.Fatalf("the retry did not get its own row: %+v", retried.Event)
	}
}

// One delivery id describing two different units of demand is a sender
// contradicting itself, not a replay: nothing is written.
func TestRecordDemandEventRejectsReusedDeliveryID(t *testing.T) {
	store, pool := integrationStore(t)
	defer pool.Close()
	ctx := context.Background()
	jobID := time.Now().UnixNano()%1000000000 + 4

	first := demandFixture(t, jobID, 1, demand.PhaseQueued, "delivery-reused")
	if _, err := store.RecordDemandEvent(ctx, first); err != nil {
		t.Fatal(err)
	}
	other := demandFixture(t, jobID+500, 1, demand.PhaseQueued, "delivery-reused")
	if _, err := store.RecordDemandEvent(ctx, other); !errors.Is(err, ErrConflict) {
		t.Fatalf("got %v, want ErrConflict", err)
	}
	if _, err := store.GetDemandEvent(ctx, other.Key()); !errors.Is(err, ErrNotFound) {
		t.Fatalf("the rejected delivery left a row behind: %v", err)
	}
}

// Many copies of the same three deliveries at once: the row still ends at
// the highest phase and every delivery is recorded exactly once.
func TestRecordDemandEventConcurrentDeliveries(t *testing.T) {
	store, pool := integrationStore(t)
	defer pool.Close()
	ctx := context.Background()
	jobID := time.Now().UnixNano()%1000000000 + 5

	events := []demand.Event{
		demandFixture(t, jobID, 1, demand.PhaseQueued, "race-q"),
		demandFixture(t, jobID, 1, demand.PhaseInProgress, "race-p"),
		demandFixture(t, jobID, 1, demand.PhaseCompleted, "race-c"),
	}
	const copies = 4
	start := make(chan struct{})
	errs := make(chan error, len(events)*copies)
	for i := 0; i < copies; i++ {
		for _, event := range events {
			go func(event demand.Event) {
				<-start
				_, err := store.RecordDemandEvent(ctx, event)
				errs <- err
			}(event)
		}
	}
	close(start)
	for i := 0; i < len(events)*copies; i++ {
		if err := <-errs; err != nil && !errors.Is(err, ErrConflict) {
			t.Fatalf("racing delivery: %v", err)
		}
	}
	record, err := store.GetDemandEvent(ctx, events[0].Key())
	if err != nil {
		t.Fatal(err)
	}
	if record.Event.Phase != demand.PhaseCompleted {
		t.Fatalf("racing deliveries settled at %s, want completed", record.Event.Phase)
	}
	var deliveries int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM demand_deliveries WHERE demand_key=$1`,
		events[0].Key()).Scan(&deliveries); err != nil {
		t.Fatal(err)
	}
	if deliveries != len(events) {
		t.Fatalf("got %d delivery rows, want %d", deliveries, len(events))
	}
}
