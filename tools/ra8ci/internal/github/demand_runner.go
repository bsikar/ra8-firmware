// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"strconv"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// demandRunnerPrefix marks a runner this control plane minted for one unit of
// demand, so a name in the scale set says where it came from.
const demandRunnerPrefix = "ra8ci-"

var (
	// ErrNotRegisterable is demand that does not want a runner: a job
	// already in progress or finished has one, or had one, and minting a
	// second credential for it would put two runners on one job.
	ErrNotRegisterable = errors.New("demand does not need a runner")
	// ErrAlreadyRegistered is a runner already carrying this unit of
	// demand's name. Registration is idempotent by refusal rather than by
	// reissue: a JIT credential is single use, so handing out a second one
	// is how a job gets picked up twice.
	ErrAlreadyRegistered = errors.New("runner already registered for this demand")
)

// DemandRunnerName is the runner name one unit of demand gets. It is derived
// from the demand identity (job id and run attempt) and nothing else, so the
// name is stable across a retried delivery, a reconciliation pass and a
// restart: asking GitHub whether this demand already has a runner is then a
// lookup rather than a search.
func DemandRunnerName(event demand.Event) (string, error) {
	if event.JobID <= 0 || event.RunAttempt < 1 {
		return "", fmt.Errorf("%w: demand identity", demand.ErrInvalid)
	}
	name := demandRunnerPrefix + strconv.FormatInt(event.JobID, 10) + "-" + strconv.Itoa(event.RunAttempt)
	if !runnerIdentityName.MatchString(name) {
		return "", fmt.Errorf("%w: runner name %q", demand.ErrInvalid, name)
	}
	return name, nil
}

// JITProvider is the slice of a session registration needs: mint a one-job
// credential, and say whether this scale set already has a runner under a
// name. *Session satisfies it.
type JITProvider interface {
	GenerateJIT(ctx context.Context, name string) (JITRunnerConfig, error)
	RunnerByName(ctx context.Context, name string) (RunnerIdentity, bool, error)
}

// DemandRegistrar turns one unit of queued demand into one single-use runner
// credential. It owns the rule that a unit of demand gets exactly one
// credential; who provisions the machine that consumes it is somebody else's
// problem.
type DemandRegistrar struct {
	provider JITProvider
}

// NewDemandRegistrar refuses a nil provider up front rather than at the first
// piece of demand.
func NewDemandRegistrar(provider JITProvider) (*DemandRegistrar, error) {
	if provider == nil {
		return nil, errors.New("demand registrar requires a JIT provider")
	}
	return &DemandRegistrar{provider: provider}, nil
}

// Register mints the credential for one unit of demand. The caller owns the
// returned buffer and must Clear it as soon as it has been delivered to the
// machine that will use it.
//
// The order matters: validate, refuse demand that does not want a runner,
// ask whether one already exists, and only then spend a credential. GitHub
// will happily mint a second JIT config for a second name, so the check that
// stops a job being taken twice has to happen before the call, not after it.
func (r *DemandRegistrar) Register(ctx context.Context, event demand.Event) (JITRunnerConfig, error) {
	if r == nil || r.provider == nil || ctx == nil {
		return JITRunnerConfig{}, errors.New("invalid runner registration")
	}
	if err := event.Validate(); err != nil {
		return JITRunnerConfig{}, err
	}
	if event.Phase != demand.PhaseQueued {
		return JITRunnerConfig{}, fmt.Errorf("%w: %s is %s", ErrNotRegisterable, event.Key(), event.Phase)
	}
	name, err := DemandRunnerName(event)
	if err != nil {
		return JITRunnerConfig{}, err
	}
	existing, exists, err := r.provider.RunnerByName(ctx, name)
	if err != nil {
		return JITRunnerConfig{}, fmt.Errorf("check runner for demand %s: %w", event.Key(), err)
	}
	if exists {
		return JITRunnerConfig{}, fmt.Errorf("%w: %s is runner %d", ErrAlreadyRegistered, name, existing.ID)
	}
	config, err := r.provider.GenerateJIT(ctx, name)
	if err != nil {
		return JITRunnerConfig{}, fmt.Errorf("register runner for demand %s: %w", event.Key(), err)
	}
	if config.Runner.Name != name {
		config.Clear()
		return JITRunnerConfig{}, fmt.Errorf("registration returned runner %q for demand %s",
			config.Runner.Name, event.Key())
	}
	return config, nil
}

// Registered reports the runner this unit of demand already has, if any. It
// is the read half of registration, for a caller reconciling what it minted
// against what the scale set holds.
func (r *DemandRegistrar) Registered(ctx context.Context, event demand.Event) (RunnerIdentity, bool, error) {
	if r == nil || r.provider == nil || ctx == nil {
		return RunnerIdentity{}, false, errors.New("invalid runner lookup")
	}
	name, err := DemandRunnerName(event)
	if err != nil {
		return RunnerIdentity{}, false, err
	}
	return r.provider.RunnerByName(ctx, name)
}
