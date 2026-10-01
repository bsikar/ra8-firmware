// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"errors"
	"testing"
	"time"
)

type recorderStub struct {
	events []Event
	err    error
}

func (r *recorderStub) Record(_ context.Context, event Event) error {
	if r.err != nil {
		return r.err
	}
	r.events = append(r.events, event)
	return nil
}

func sampleEvent(adapter string) Event {
	queued := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	return Event{
		Adapter:    adapter,
		DeliveryID: "delivery-1",
		Phase:      PhaseQueued,
		JobID:      991,
		RunID:      77,
		RunAttempt: 1,
		Owner:      "bsikar",
		Repository: "ra8-firmware",
		Workflow:   "ci",
		JobName:    "build",
		CommitSHA:  "0123456789abcdef0123456789abcdef01234567",
		Labels:     []string{"ra8-lab"},
		QueuedAt:   queued,
		ObservedAt: queued.Add(time.Second),
	}
}

func TestRegistryAdmitsTheDefaultsAndNothingElse(t *testing.T) {
	registry, err := NewRegistry(&recorderStub{})
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	for _, adapter := range DefaultAdapters() {
		if !registry.Enabled(adapter) {
			t.Fatalf("default adapter %q is not enabled", adapter)
		}
	}
	if registry.Enabled(ScaleSetAdapter) {
		t.Fatal("the scale-set adapter must be off until it is named")
	}
	if registry.Enabled("") {
		t.Fatal("an empty adapter name must never be enabled")
	}
}

func TestRegistryEnablesANamedSecondAdapter(t *testing.T) {
	recorder := &recorderStub{}
	registry, err := NewRegistry(recorder, ScaleSetAdapter, WebhookAdapter)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	if !registry.Enabled(ScaleSetAdapter) {
		t.Fatal("a named adapter must be enabled")
	}
	want := []string{ReconcileAdapter, ScaleSetAdapter, WebhookAdapter}
	got := registry.Adapters()
	if len(got) != len(want) {
		t.Fatalf("adapters = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("adapters = %v, want %v", got, want)
		}
	}
	if err := registry.Submit(context.Background(), sampleEvent(ScaleSetAdapter)); err != nil {
		t.Fatalf("submit: %v", err)
	}
	if len(recorder.events) != 1 {
		t.Fatalf("recorded %d events, want 1", len(recorder.events))
	}
}

func TestRegistryRefusesDemandFromADisabledAdapter(t *testing.T) {
	recorder := &recorderStub{}
	registry, err := NewRegistry(recorder)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	err = registry.Submit(context.Background(), sampleEvent(ScaleSetAdapter))
	if !errors.Is(err, ErrAdapterDisabled) {
		t.Fatalf("submit from a disabled adapter = %v, want ErrAdapterDisabled", err)
	}
	if len(recorder.events) != 0 {
		t.Fatal("a disabled adapter reached the recorder")
	}
}

func TestRegistryValidatesBeforeItWrites(t *testing.T) {
	recorder := &recorderStub{}
	registry, err := NewRegistry(recorder)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	event := sampleEvent(WebhookAdapter)
	event.CommitSHA = "not-a-sha"
	if err := registry.Submit(context.Background(), event); !errors.Is(err, ErrInvalid) {
		t.Fatalf("submit of an invalid event = %v, want ErrInvalid", err)
	}
	if len(recorder.events) != 0 {
		t.Fatal("an invalid event reached the recorder")
	}
	blank := sampleEvent("")
	if err := registry.Submit(context.Background(), blank); !errors.Is(err, ErrInvalid) {
		t.Fatalf("submit without an adapter = %v, want ErrInvalid", err)
	}
}

func TestRegistrySurfacesTheRecorderFailure(t *testing.T) {
	sentinel := errors.New("store down")
	registry, err := NewRegistry(&recorderStub{err: sentinel})
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	if err := registry.Submit(context.Background(), sampleEvent(WebhookAdapter)); !errors.Is(err, sentinel) {
		t.Fatalf("submit = %v, want the recorder failure", err)
	}
}

func TestNewRegistryRefusesNonsense(t *testing.T) {
	if _, err := NewRegistry(nil); err == nil {
		t.Fatal("a registry without a recorder must be refused")
	}
	if _, err := NewRegistry(&recorderStub{}, ""); !errors.Is(err, ErrInvalid) {
		t.Fatal("an empty extra adapter name must be refused")
	}
	long := make([]byte, 65)
	for i := range long {
		long[i] = 'a'
	}
	if _, err := NewRegistry(&recorderStub{}, string(long)); !errors.Is(err, ErrInvalid) {
		t.Fatal("an over-long adapter name must be refused")
	}
	var nilRegistry *Registry
	if nilRegistry.Enabled(WebhookAdapter) {
		t.Fatal("a nil registry must enable nothing")
	}
	if err := nilRegistry.Submit(context.Background(), sampleEvent(WebhookAdapter)); !errors.Is(err, ErrInvalid) {
		t.Fatal("a nil registry must refuse rather than panic")
	}
}

func TestRegistryRecordIsSubmitUnderTheRecorderName(t *testing.T) {
	recorder := &recorderStub{}
	registry, err := NewRegistry(recorder)
	if err != nil {
		t.Fatalf("new registry: %v", err)
	}
	var sink EventRecorder = registry
	if err := sink.Record(context.Background(), sampleEvent(WebhookAdapter)); err != nil {
		t.Fatalf("record: %v", err)
	}
	if len(recorder.events) != 1 {
		t.Fatalf("recorded %d events, want 1", len(recorder.events))
	}
	if err := sink.Record(context.Background(), sampleEvent(ScaleSetAdapter)); !errors.Is(err, ErrAdapterDisabled) {
		t.Fatalf("record from a disabled adapter = %v, want ErrAdapterDisabled", err)
	}
}
