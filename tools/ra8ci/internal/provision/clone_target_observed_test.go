// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
)

func cloneTargetIdentity() proxmox.Identity {
	return proxmox.Identity{
		VMID:                9000,
		Node:                "ra8-lab-1",
		Pool:                "ra8-tf-lab",
		Storage:             "ra8-tf-lab",
		Name:                "ra8-lab-ci-9000",
		ReservationID:       "2f5a13ed-f770-4f68-a246-52c1e8f7e018",
		CreationOperationID: "4e172436-ec32-4a3d-ad27-c6b0efed58f4",
	}
}

func observing(vm proxmox.VM, err error) func(context.Context, proxmox.Identity) (proxmox.VM, error) {
	return func(context.Context, proxmox.Identity) (proxmox.VM, error) { return vm, err }
}

func TestAnAbsentTargetVMIDIsFreeToClone(t *testing.T) {
	err := checkCloneTargetIsFree(context.Background(),
		observing(proxmox.VM{}, proxmox.ErrNotFound), cloneTargetIdentity())
	if err != nil {
		t.Fatalf("refused a clone into an unoccupied VMID: %v", err)
	}
}

func TestAWrappedNotFoundIsStillAnAbsentTarget(t *testing.T) {
	err := checkCloneTargetIsFree(context.Background(),
		observing(proxmox.VM{}, fmt.Errorf("read cluster resources: %w", proxmox.ErrNotFound)),
		cloneTargetIdentity())
	if err != nil {
		t.Fatalf("refused a clone whose target was reported absent through a wrapped error: %v", err)
	}
}

func TestACloneReplayIsNotRefused(t *testing.T) {
	// A successful observation means the guest at that VMID carries this
	// reservation's own name and marker: the clone already landed and this is
	// the replay after a crash. Refusing it here would strand the reservation.
	identity := cloneTargetIdentity()
	err := checkCloneTargetIsFree(context.Background(),
		observing(proxmox.VM{Identity: identity, Status: "stopped"}, nil), identity)
	if err != nil {
		t.Fatalf("refused the replay of a clone that already landed: %v", err)
	}
}

func TestAForeignGuestOnTheTargetVMIDIsRefused(t *testing.T) {
	err := checkCloneTargetIsFree(context.Background(),
		observing(proxmox.VM{}, proxmox.ErrConflict), cloneTargetIdentity())
	if err == nil {
		t.Fatal("cloned into a VMID occupied by another reservation")
	}
	if !errors.Is(err, proxmox.ErrConflict) {
		t.Fatalf("refusal lost the conflict it was built on: %v", err)
	}
}

func TestARefusalNamesTheVMIDAndTheReservation(t *testing.T) {
	identity := cloneTargetIdentity()
	err := checkCloneTargetIsFree(context.Background(),
		observing(proxmox.VM{}, proxmox.ErrConflict), identity)
	if err == nil {
		t.Fatal("expected a refusal")
	}
	message := err.Error()
	for _, want := range []string{"9000", identity.ReservationID} {
		if !strings.Contains(message, want) {
			t.Fatalf("refusal %q does not name %q", message, want)
		}
	}
}

func TestAnObservationThatFailedIsNotReadAsAFreeVMID(t *testing.T) {
	// The check exists to decide before a one-way gate, so an answer nobody
	// got is not an answer that the VMID is unoccupied.
	err := checkCloneTargetIsFree(context.Background(),
		observing(proxmox.VM{}, errors.New("dial tcp: connection refused")), cloneTargetIdentity())
	if err == nil {
		t.Fatal("treated a failed observation as an unoccupied VMID")
	}
}

func TestAnInvalidIdentityIsRefusedRatherThanCloned(t *testing.T) {
	err := checkCloneTargetIsFree(context.Background(),
		observing(proxmox.VM{}, proxmox.ErrInvalid), cloneTargetIdentity())
	if err == nil || !errors.Is(err, proxmox.ErrInvalid) {
		t.Fatalf("an identity the observer refuses must not clone: %v", err)
	}
}

func TestACloneWithNoObserverIsRefused(t *testing.T) {
	if err := checkCloneTargetIsFree(context.Background(), nil, cloneTargetIdentity()); err == nil {
		t.Fatal("cloned with no way to observe the target VMID")
	}
}
