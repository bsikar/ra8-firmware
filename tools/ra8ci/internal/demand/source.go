// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"errors"
	"fmt"
	"sort"
)

// ScaleSetAdapter names demand observed through the Actions scale-set client.
// It is a second way the same units of demand reach this plane, never a
// second identity for them: a job seen by both the App and the scale set
// lands on one row, because the key is the job and the run attempt.
const ScaleSetAdapter = "actions-scale-set"

// ErrAdapterDisabled is demand from an adapter this plane is not accepting.
// It is a configuration answer, not a fault: the caller should drop the
// event, not retry it.
var ErrAdapterDisabled = errors.New("demand adapter disabled")

// Source is one way demand reaches the control plane. The interface is
// deliberately small: what an adapter reads is its own business, and the only
// thing the plane needs from it is which name its evidence carries and
// whether it is switched on.
type Source interface {
	// Adapter is the name this source writes as. It is evidence about how
	// a copy of an event arrived and never takes part in identity.
	Adapter() string
	// Enabled reports whether this plane currently accepts its demand.
	Enabled() bool
}

// Registry is the admission point every adapter writes through. It exists so
// that turning a second adapter on is one named decision in one place rather
// than a call site that happens to exist.
type Registry struct {
	recorder EventRecorder
	enabled  map[string]bool
}

// DefaultAdapters are the adapters a registry admits without being asked for
// them: the GitHub App front door, and the reconciliation pass that covers
// the deliveries it drops. Every other adapter, the scale-set client
// included, is off until a caller names it.
func DefaultAdapters() []string { return []string{WebhookAdapter, ReconcileAdapter} }

// NewRegistry admits the default adapters plus whichever extras the caller
// names. Naming one twice, or naming a default, is not an error: the set is
// what matters, not how it was reached.
func NewRegistry(recorder EventRecorder, extra ...string) (*Registry, error) {
	if recorder == nil {
		return nil, errors.New("demand registry requires an event recorder")
	}
	enabled := make(map[string]bool, len(DefaultAdapters())+len(extra))
	for _, adapter := range DefaultAdapters() {
		enabled[adapter] = true
	}
	for _, adapter := range extra {
		if adapter == "" || len(adapter) > 64 {
			return nil, fmt.Errorf("%w: adapter name", ErrInvalid)
		}
		enabled[adapter] = true
	}
	return &Registry{recorder: recorder, enabled: enabled}, nil
}

// Enabled reports whether this registry accepts demand from an adapter.
func (r *Registry) Enabled(adapter string) bool {
	return r != nil && adapter != "" && r.enabled[adapter]
}

// Adapters lists what this registry accepts, sorted, so a startup line can
// say out loud which sources are live.
func (r *Registry) Adapters() []string {
	if r == nil {
		return nil
	}
	names := make([]string, 0, len(r.enabled))
	for adapter := range r.enabled {
		names = append(names, adapter)
	}
	sort.Strings(names)
	return names
}

// Submit records one normalized event. The gate is checked before the event
// is even validated, so a disabled adapter cannot reach the store through a
// well-formed payload, and validation runs before the write, so the store is
// never asked to hold demand no adapter was allowed to produce.
func (r *Registry) Submit(ctx context.Context, event Event) error {
	switch {
	case r == nil || r.recorder == nil:
		return fmt.Errorf("%w: registry", ErrInvalid)
	case event.Adapter == "":
		return fmt.Errorf("%w: adapter", ErrInvalid)
	case !r.Enabled(event.Adapter):
		return fmt.Errorf("%w: %s", ErrAdapterDisabled, event.Adapter)
	}
	if err := event.Validate(); err != nil {
		return err
	}
	return r.recorder.Record(ctx, event)
}

// Record satisfies EventRecorder, so the webhook endpoint and anything else
// that writes one event at a time can be pointed at the gate instead of at
// the store directly. It is Submit under the name the rest of the package
// already uses for a recorder.
func (r *Registry) Record(ctx context.Context, event Event) error { return r.Submit(ctx, event) }

var _ EventRecorder = (*Registry)(nil)
