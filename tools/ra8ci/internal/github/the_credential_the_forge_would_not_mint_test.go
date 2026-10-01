// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// Minting is the step that spends something: a credential the scale set will
// honour exactly once. When the forge refuses to mint, the refusal has to name
// the demand it was for, because the caller's next move is to decide whether
// that unit of demand is still owed a runner, and nothing else in the answer
// says which unit it was.
func TestACredentialTheForgeWouldNotMintNamesItsDemand(t *testing.T) {
	registrar, provider := registrarFixture(t)
	provider.mintErr = errors.New("scale set is at its runner ceiling")

	event := demandFixture(demand.PhaseQueued)
	config, err := registrar.Register(context.Background(), event)
	if err == nil {
		t.Fatal("a refused mint answered a credential")
	}
	if !strings.Contains(err.Error(), "register runner for demand "+event.Key()) {
		t.Fatalf("a refused mint answered %v, want the demand named", err)
	}
	if !strings.Contains(err.Error(), "scale set is at its runner ceiling") {
		t.Fatalf("a refused mint hid the forge's own reason: %v", err)
	}
	if len(config.EncodedConfig) != 0 || config.Runner.Name != "" {
		t.Fatalf("a refused mint answered %+v", config)
	}
	// The lookup ran and the mint was attempted once: a refusal is not a
	// reason to try again inside one registration.
	if len(provider.minted) != 1 {
		t.Fatalf("the forge was asked to mint %d times", len(provider.minted))
	}
	if provider.minted[0] != demandRunnerPrefix+"4429117744-2" {
		t.Fatalf("the mint asked for %q", provider.minted[0])
	}
}

// A refusal is not an ErrAlreadyRegistered: the demand has no runner, and a
// caller reading it as one would leave the job queued forever.
func TestARefusedMintIsNotReadAsAnExistingRunner(t *testing.T) {
	registrar, provider := registrarFixture(t)
	provider.mintErr = errors.New("forge is unavailable")

	_, err := registrar.Register(context.Background(), demandFixture(demand.PhaseQueued))
	if errors.Is(err, ErrAlreadyRegistered) || errors.Is(err, ErrNotRegisterable) {
		t.Fatalf("a refused mint answered %v, which reads as a decided registration", err)
	}
	if errors.Is(err, demand.ErrInvalid) {
		t.Fatalf("a refused mint answered %v, which reads as unusable demand", err)
	}
}

// A credential minted under another name is discarded rather than handed
// back: it would run some other job's work on this demand's machine.
func TestACredentialMintedUnderAnotherNameIsDiscarded(t *testing.T) {
	registrar, provider := registrarFixture(t)
	provider.mintName = demandRunnerPrefix + "4429117744-3"

	event := demandFixture(demand.PhaseQueued)
	config, err := registrar.Register(context.Background(), event)
	if err == nil {
		t.Fatal("a credential under another name was handed back")
	}
	if !strings.Contains(err.Error(), provider.mintName) || !strings.Contains(err.Error(), event.Key()) {
		t.Fatalf("the refusal answered %v, want the returned name and the demand", err)
	}
	if len(config.EncodedConfig) != 0 {
		t.Fatalf("a discarded credential still carried %d bytes of configuration", len(config.EncodedConfig))
	}
}
