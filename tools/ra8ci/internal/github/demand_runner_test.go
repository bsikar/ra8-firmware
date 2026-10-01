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

type jitProviderFake struct {
	existing  map[string]RunnerIdentity
	minted    []string
	lookupErr error
	mintErr   error
	mintName  string
	nextID    int
}

func (f *jitProviderFake) RunnerByName(_ context.Context, name string) (RunnerIdentity, bool, error) {
	if f.lookupErr != nil {
		return RunnerIdentity{}, false, f.lookupErr
	}
	identity, ok := f.existing[name]
	return identity, ok, nil
}

func (f *jitProviderFake) GenerateJIT(_ context.Context, name string) (JITRunnerConfig, error) {
	f.minted = append(f.minted, name)
	if f.mintErr != nil {
		return JITRunnerConfig{}, f.mintErr
	}
	returned := name
	if f.mintName != "" {
		returned = f.mintName
	}
	f.nextID++
	return JITRunnerConfig{Runner: RunnerIdentity{ID: f.nextID, Name: returned},
		EncodedConfig: []byte("credential")}, nil
}

func demandFixture(phase demand.Phase) demand.Event {
	queued := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	event := demand.Event{Adapter: demand.WebhookAdapter, DeliveryID: "d-1", Phase: phase,
		JobID: 4429117744, RunID: 91, RunAttempt: 2, Owner: "bsikar", Repository: "ra8-firmware",
		Workflow: "ci", JobName: "build", CommitSHA: "0123456789abcdef0123456789abcdef01234567",
		Labels: []string{"self-hosted", "ra8"}, QueuedAt: queued, ObservedAt: queued.Add(time.Second)}
	if phase != demand.PhaseQueued {
		event.StartedAt = queued.Add(time.Minute)
	}
	if phase == demand.PhaseCompleted {
		event.Conclusion = "success"
		event.CompletedAt = queued.Add(2 * time.Minute)
	}
	return event
}

func registrarFixture(t *testing.T) (*DemandRegistrar, *jitProviderFake) {
	t.Helper()
	provider := &jitProviderFake{existing: map[string]RunnerIdentity{}}
	registrar, err := NewDemandRegistrar(provider)
	if err != nil {
		t.Fatalf("new registrar: %v", err)
	}
	return registrar, provider
}

// The name is the only thing that lets a restart ask GitHub whether this
// demand already has a runner, so it has to come from the demand identity
// and nothing else, and it has to survive the session's own name rules.
func TestDemandRunnerNameIsStableAndAcceptable(t *testing.T) {
	event := demandFixture(demand.PhaseQueued)
	first, err := DemandRunnerName(event)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	event.DeliveryID, event.Adapter, event.RunnerName = "d-2", demand.ReconcileAdapter, "someone-else"
	second, err := DemandRunnerName(event)
	if err != nil {
		t.Fatalf("name after re-delivery: %v", err)
	}
	if first != second {
		t.Fatalf("name moved with the delivery: %q then %q", first, second)
	}
	if !runnerIdentityName.MatchString(first) {
		t.Fatalf("name %q is not one GenerateJIT will accept", first)
	}
	// A different run attempt is different demand and must not collide.
	event.RunAttempt = 3
	third, err := DemandRunnerName(event)
	if err != nil {
		t.Fatalf("name for the retry: %v", err)
	}
	if third == first {
		t.Fatalf("two run attempts share runner name %q", third)
	}
	// The widest identity GitHub can hand us still has to fit.
	widest, err := DemandRunnerName(demand.Event{JobID: 9223372036854775807, RunAttempt: 1000})
	if err != nil {
		t.Fatalf("widest identity: %v", err)
	}
	if !runnerIdentityName.MatchString(widest) {
		t.Fatalf("widest name %q is unacceptable", widest)
	}
}

func TestDemandRunnerNameRefusesUnidentifiedDemand(t *testing.T) {
	for _, event := range []demand.Event{{JobID: 0, RunAttempt: 1}, {JobID: 5, RunAttempt: 0}} {
		if _, err := DemandRunnerName(event); !errors.Is(err, demand.ErrInvalid) {
			t.Fatalf("unidentified demand should be invalid, got %v", err)
		}
	}
}

func TestRegisterMintsOneCredentialForQueuedDemand(t *testing.T) {
	registrar, provider := registrarFixture(t)
	event := demandFixture(demand.PhaseQueued)
	config, err := registrar.Register(context.Background(), event)
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	name, _ := DemandRunnerName(event)
	if config.Runner.Name != name || len(config.EncodedConfig) == 0 {
		t.Fatalf("registration returned %+v", config.Runner)
	}
	if len(provider.minted) != 1 || provider.minted[0] != name {
		t.Fatalf("minted %v, want one credential for %q", provider.minted, name)
	}
	config.Clear()
}

// A JIT credential is single use, so a second one for the same demand is how
// a job gets picked up twice. Registration refuses instead of reissuing.
func TestRegisterRefusesDemandThatAlreadyHasARunner(t *testing.T) {
	registrar, provider := registrarFixture(t)
	event := demandFixture(demand.PhaseQueued)
	name, _ := DemandRunnerName(event)
	provider.existing[name] = RunnerIdentity{ID: 77, Name: name}
	_, err := registrar.Register(context.Background(), event)
	if !errors.Is(err, ErrAlreadyRegistered) {
		t.Fatalf("second registration should refuse, got %v", err)
	}
	if len(provider.minted) != 0 {
		t.Fatalf("a credential was spent anyway: %v", provider.minted)
	}
}

// Demand that is already running, or over, has had its runner. Minting for
// it puts a second runner on a job somebody else is doing.
func TestRegisterRefusesDemandPastQueued(t *testing.T) {
	for _, phase := range []demand.Phase{demand.PhaseInProgress, demand.PhaseCompleted} {
		registrar, provider := registrarFixture(t)
		_, err := registrar.Register(context.Background(), demandFixture(phase))
		if !errors.Is(err, ErrNotRegisterable) {
			t.Fatalf("phase %s should not be registerable, got %v", phase, err)
		}
		if len(provider.minted) != 0 {
			t.Fatalf("phase %s spent a credential: %v", phase, provider.minted)
		}
	}
}

func TestRegisterRefusesInvalidDemandBeforeTouchingTheForge(t *testing.T) {
	registrar, provider := registrarFixture(t)
	event := demandFixture(demand.PhaseQueued)
	event.CommitSHA = "not-a-sha"
	if _, err := registrar.Register(context.Background(), event); !errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("invalid demand should be refused, got %v", err)
	}
	if len(provider.minted) != 0 {
		t.Fatalf("invalid demand reached the forge: %v", provider.minted)
	}
}

// A lookup that fails is not a missing runner: treating it as one would mint
// a second credential exactly when the plane cannot see the first.
func TestRegisterDoesNotMintWhenTheLookupFails(t *testing.T) {
	registrar, provider := registrarFixture(t)
	provider.lookupErr = errors.New("forge unreachable")
	if _, err := registrar.Register(context.Background(), demandFixture(demand.PhaseQueued)); err == nil {
		t.Fatal("a failed lookup must not read as an unregistered runner")
	}
	if len(provider.minted) != 0 {
		t.Fatalf("minted despite an unreadable scale set: %v", provider.minted)
	}
}

// A credential for a name we did not ask for belongs to some other job.
func TestRegisterRejectsAMismatchedIdentity(t *testing.T) {
	registrar, provider := registrarFixture(t)
	provider.mintName = "ra8ci-1-1"
	if _, err := registrar.Register(context.Background(), demandFixture(demand.PhaseQueued)); err == nil {
		t.Fatal("a mismatched runner identity must not be returned as this demand's credential")
	}
}

func TestRegisteredReadsTheDemandsRunner(t *testing.T) {
	registrar, provider := registrarFixture(t)
	event := demandFixture(demand.PhaseQueued)
	name, _ := DemandRunnerName(event)
	if _, exists, err := registrar.Registered(context.Background(), event); err != nil || exists {
		t.Fatalf("unregistered demand: exists=%v err=%v", exists, err)
	}
	provider.existing[name] = RunnerIdentity{ID: 9, Name: name}
	identity, exists, err := registrar.Registered(context.Background(), event)
	if err != nil || !exists || identity.ID != 9 {
		t.Fatalf("registered demand: %+v exists=%v err=%v", identity, exists, err)
	}
}

func TestNewDemandRegistrarRequiresAProvider(t *testing.T) {
	if _, err := NewDemandRegistrar(nil); err == nil {
		t.Fatal("a registrar without a provider must not be constructed")
	}
	var registrar *DemandRegistrar
	if _, err := registrar.Register(context.Background(), demandFixture(demand.PhaseQueued)); err == nil {
		t.Fatal("a nil registrar must refuse rather than panic")
	}
}

// *Session is the production provider; the interface must keep matching it.
var _ JITProvider = (*Session)(nil)
