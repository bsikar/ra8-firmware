// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

// The first step of the unclaimed-runner sequence, on the forge side.
//
// A just-in-time credential is minted before the guest that consumes it
// exists. If the job is cancelled in that window, the registration is still
// live and the scale set will hand it a queued job the moment anything comes
// up carrying it. That is why revocation happens before the guest is
// destroyed: a guest torn down underneath a job it has just accepted takes
// that job down with it.
//
// Everything here is idempotent. The reaper resumes a half-finished
// reservation by walking the sequence from the top, so this is asked to
// revoke registrations that are already gone, and answers that nothing was
// there rather than failing.

// ErrForeignRunner is a runner this control plane did not mint, or one whose
// identity does not match the reservation asking for its removal. It is a
// refusal, never a retry: something else owns that runner.
var ErrForeignRunner = errors.New("refusing to revoke a runner this plane does not own")

// RunnerRef is what a reservation knows about the registration it was issued.
// Either half may be missing: a reservation that never got as far as
// registering carries neither, and one that recorded a name before the
// external id came back carries only the name.
type RunnerRef struct {
	ID   int
	Name string
}

// Registered reports whether anything was ever registered for this
// reservation. A zero ref is the normal case for demand cancelled between the
// reservation and the mint, and is not an error anywhere in the sequence.
func (r RunnerRef) Registered() bool {
	return r.ID > 0 || r.Name != ""
}

// Minted reports whether a runner name is one this plane issued. Nothing is
// removed from the scale set without it: the name is derived from demand
// identity and carries the prefix, so a runner without it belongs to someone
// else, whatever a stale reservation row says.
func Minted(name string) bool {
	return strings.HasPrefix(name, demandRunnerPrefix) && runnerIdentityName.MatchString(name)
}

// RunnerRegistry is the slice of a session revocation needs. *Session
// satisfies it.
type RunnerRegistry interface {
	RunnerByName(ctx context.Context, name string) (RunnerIdentity, bool, error)
	RunnerByID(ctx context.Context, id int) (RunnerIdentity, bool, error)
	RemoveRunner(ctx context.Context, id int) error
}

var _ RunnerRegistry = (*Session)(nil)

// RegistrationRevoker removes the registration a reservation was issued, so
// nothing can accept a job on a credential nobody took.
type RegistrationRevoker struct {
	runners RunnerRegistry
}

// NewRegistrationRevoker refuses a nil registry up front rather than at the
// first expired reservation, which is the worst moment to discover it.
func NewRegistrationRevoker(runners RunnerRegistry) (*RegistrationRevoker, error) {
	if runners == nil {
		return nil, errors.New("registration revoker requires a runner registry")
	}
	return &RegistrationRevoker{runners: runners}, nil
}

// Revoke removes the runner the reference names and reports whether there was
// one to remove. Three outcomes, all of them normal:
//
//   - nothing was ever registered, or the registration is already gone:
//     false, nil. A resumed pass sees this on its second time through.
//   - a runner was there and is now removed: true, nil.
//   - the runner on file is not this plane's, or its identity disagrees with
//     the reference: ErrForeignRunner, and nothing is removed.
//
// The lookup comes first even though RemoveRunner performs one of its own:
// the reference is what the ledger believes, and the point of checking is to
// refuse when the forge disagrees with it, before anything is removed.
func (r *RegistrationRevoker) Revoke(ctx context.Context, ref RunnerRef) (bool, error) {
	if r == nil || r.runners == nil || ctx == nil {
		return false, errors.New("invalid registration revocation")
	}
	if !ref.Registered() {
		return false, nil
	}
	runner, exists, err := r.lookup(ctx, ref)
	if err != nil {
		return false, err
	}
	if !exists {
		return false, nil
	}
	if !Minted(runner.Name) {
		return false, fmt.Errorf("%w: runner %d is named %q", ErrForeignRunner, runner.ID, runner.Name)
	}
	if ref.ID > 0 && runner.ID != ref.ID {
		return false, fmt.Errorf("%w: reservation holds runner %d, the forge holds %d",
			ErrForeignRunner, ref.ID, runner.ID)
	}
	if ref.Name != "" && runner.Name != ref.Name {
		return false, fmt.Errorf("%w: reservation holds runner %q, the forge holds %q",
			ErrForeignRunner, ref.Name, runner.Name)
	}
	if err := r.runners.RemoveRunner(ctx, runner.ID); err != nil {
		return false, fmt.Errorf("revoke runner %d: %w", runner.ID, err)
	}
	return true, nil
}

// lookup prefers the name, because the name is derived from demand identity
// and survives a forge that reissued the id. A reference carrying only an id
// is looked up by id, and either lookup returning nothing means the
// registration is already gone.
func (r *RegistrationRevoker) lookup(ctx context.Context, ref RunnerRef) (RunnerIdentity, bool, error) {
	if ref.Name != "" {
		if !Minted(ref.Name) {
			return RunnerIdentity{}, false, fmt.Errorf("%w: reservation names %q", ErrForeignRunner, ref.Name)
		}
		runner, exists, err := r.runners.RunnerByName(ctx, ref.Name)
		if err != nil {
			return RunnerIdentity{}, false, fmt.Errorf("look up runner %q: %w", ref.Name, err)
		}
		return runner, exists, nil
	}
	runner, exists, err := r.runners.RunnerByID(ctx, ref.ID)
	if err != nil {
		return RunnerIdentity{}, false, fmt.Errorf("look up runner %d: %w", ref.ID, err)
	}
	return runner, exists, nil
}
