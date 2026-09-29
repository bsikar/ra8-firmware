// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// A reconciliation pass runs unattended against a live forge, so what it
// refuses matters as much as what it records: a half-configured pass, a store
// it cannot read, and demand on file that cannot be concluded are each an
// answer the operator has to be given rather than a silent zero report.

// refusingDemand is a store that cannot be read. Record is never reached.
type refusingDemand struct{ refusal error }

func (r *refusingDemand) ListOpen(ctx context.Context, limit int) ([]Event, error) {
	return nil, r.refusal
}

func (r *refusingDemand) Record(ctx context.Context, event Event) error {
	return errors.New("a store that cannot be listed was written to")
}

// A pass is configured once and then runs on its own, so a configuration that
// cannot work is refused at construction rather than discovered mid-pass.
func TestAPassThatCannotWorkIsRefusedAtConstruction(t *testing.T) {
	workable := ReconcilerConfig{Source: &fakeJobs{}, Store: newMemoryDemand(),
		Grace: time.Minute, MissingAfter: time.Hour, BatchSize: 50}

	for _, refused := range []struct {
		named string
		spoil func(*ReconcilerConfig)
		says  string
	}{
		{"no source", func(c *ReconcilerConfig) { c.Source = nil }, "job source"},
		{"no store", func(c *ReconcilerConfig) { c.Store = nil }, "demand store"},
		{"an adapter name too long", func(c *ReconcilerConfig) { c.Adapter = strings.Repeat("a", 65) }, "adapter name is too long"},
		{"a negative grace", func(c *ReconcilerConfig) { c.Grace = -time.Second }, "grace"},
		{"a grace past the hour", func(c *ReconcilerConfig) { c.Grace = 2 * time.Hour }, "grace"},
		{"missing-after below the grace", func(c *ReconcilerConfig) { c.MissingAfter = time.Second }, "missing-after"},
		{"missing-after past the day", func(c *ReconcilerConfig) { c.MissingAfter = 25 * time.Hour }, "missing-after"},
		{"no batch", func(c *ReconcilerConfig) { c.BatchSize = 0 }, "batch size"},
		{"a batch past the thousandth", func(c *ReconcilerConfig) { c.BatchSize = 1001 }, "batch size"},
	} {
		t.Run(refused.named, func(t *testing.T) {
			config := workable
			refused.spoil(&config)
			reconciler, err := NewReconciler(config)
			if err == nil {
				t.Fatal("a pass that cannot work against a live forge was built anyway")
			}
			if !strings.Contains(err.Error(), refused.says) {
				t.Fatalf("error = %v, want it to name %q", err, refused.says)
			}
			if reconciler != nil {
				t.Fatal("a refused configuration returned a reconciler")
			}
		})
	}
}

// The default adapter name is taken when none is configured, so a pass's own
// deliveries are attributable without every caller stating it.
func TestAPassNamesItselfWhenNoAdapterIsConfigured(t *testing.T) {
	base := time.Date(2026, 9, 28, 12, 0, 0, 0, time.UTC)
	held := queuedDemand(42, 1, base.Add(-2*time.Hour))
	store := newMemoryDemand(held)
	reconciler := reconcilerFor(t, &fakeJobs{}, store, base)

	if _, err := reconciler.Pass(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(store.recorded) != 1 {
		t.Fatalf("the pass recorded %d events, want one", len(store.recorded))
	}
	written := store.recorded[0]
	if written.Adapter != ReconcileAdapter {
		t.Fatalf("adapter = %q, want the pass's own %q", written.Adapter, ReconcileAdapter)
	}
	if !strings.HasPrefix(written.DeliveryID, "reconcile.") {
		t.Fatalf("delivery id = %q, want one a pass minted", written.DeliveryID)
	}
	// The demand the pass wrote is no longer open, which is the point of
	// concluding it: the store's own listing passes over a completion.
	open, err := store.ListOpen(context.Background(), 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(open) != 0 {
		t.Fatalf("open demand = %+v, want the concluded unit closed", open)
	}
}

// A pass with nothing to run on is refused rather than reporting a clean
// scan of nothing, which is what an operator would otherwise read.
func TestAPassWithNothingToRunOnIsRefused(t *testing.T) {
	t.Run("no reconciler", func(t *testing.T) {
		var absent *Reconciler
		if _, err := absent.Pass(context.Background()); err == nil ||
			!strings.Contains(err.Error(), "invalid reconciliation pass") {
			t.Fatalf("error = %v, want the pass refused", err)
		}
	})

	t.Run("no context", func(t *testing.T) {
		reconciler := reconcilerFor(t, &fakeJobs{}, newMemoryDemand(), time.Now())
		//lint:ignore SA1012 the nil context is the condition under test
		if _, err := reconciler.Pass(nil); err == nil ||
			!strings.Contains(err.Error(), "invalid reconciliation pass") {
			t.Fatalf("error = %v, want the pass refused", err)
		}
	})
}

// A store that cannot be read ends the pass then and there: the forge is
// never asked, and the report is empty rather than partly filled in.
func TestAStoreThatCannotBeListedEndsThePass(t *testing.T) {
	source := &fakeJobs{}
	reconciler := reconcilerFor(t, source, &refusingDemand{refusal: errors.New("connection refused")}, time.Now())

	report, err := reconciler.Pass(context.Background())
	if err == nil {
		t.Fatal("a pass that could not read its own open demand reported success")
	}
	if !strings.Contains(err.Error(), "list open demand") ||
		!strings.Contains(err.Error(), "connection refused") {
		t.Fatalf("error = %v, want the listing named and the cause carried", err)
	}
	if report != (Report{}) {
		t.Fatalf("report = %+v, want nothing counted", report)
	}
}

// Demand on file that cannot be made into a completion is counted as failed
// and carried out of the pass, rather than written as something Validate
// would have refused. The rest of the batch is still walked.
func TestDemandThatCannotBeConcludedIsCountedNotWritten(t *testing.T) {
	base := time.Date(2026, 9, 28, 12, 0, 0, 0, time.UTC)
	spoiled := queuedDemand(42, 1, base.Add(-3*time.Hour))
	spoiled.Labels = nil
	healthy := queuedDemand(43, 1, base.Add(-3*time.Hour))

	store := newMemoryDemand(spoiled, healthy)
	// fakeJobs with no snapshots knows about neither job, so both are past
	// MissingAfter and both would be concluded.
	reconciler := reconcilerFor(t, &fakeJobs{}, store, base)

	report, err := reconciler.Pass(context.Background())
	if err == nil {
		t.Fatal("a unit of demand that cannot be concluded was passed over in silence")
	}
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("error = %v, want the validation refusal carried out of the pass", err)
	}
	if !strings.Contains(err.Error(), "labels") {
		t.Fatalf("error = %v, want it to name the field at fault", err)
	}
	if report.Failed != 1 {
		t.Fatalf("failed = %d, want the one unit that could not be concluded", report.Failed)
	}
	if report.Concluded != 1 {
		t.Fatalf("concluded = %d, want the rest of the batch still walked", report.Concluded)
	}
	if report.Scanned != 2 {
		t.Fatalf("scanned = %d, want both units read", report.Scanned)
	}
}

// A registry that is not there accepts nothing and lists nothing, rather than
// answering as though every adapter were enabled.
func TestARegistryThatIsNotThereAcceptsNothing(t *testing.T) {
	var absent *Registry

	if absent.Adapters() != nil {
		t.Fatalf("adapters = %v, want none", absent.Adapters())
	}
	if absent.Enabled(WebhookAdapter) {
		t.Fatal("a registry that is not there accepted demand")
	}

	live, err := NewRegistry(&countingRecorder{}, "lab-poller")
	if err != nil {
		t.Fatal(err)
	}
	listed := live.Adapters()
	if len(listed) == 0 {
		t.Fatal("a live registry lists no adapters")
	}
	for i := 1; i < len(listed); i++ {
		if listed[i-1] >= listed[i] {
			t.Fatalf("adapters = %v, want them sorted so a startup line can read them", listed)
		}
	}
	if !live.Enabled("lab-poller") {
		t.Fatal("a registry refused the extra adapter it was built with")
	}
	if live.Enabled("") || live.Enabled("never-configured") {
		t.Fatal("a registry accepted an adapter it was never given")
	}
}
