// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
)

// checkCloneTargetIsFree observes the VMID a clone is about to create before
// the apply intent is consumed.
//
// Start, Stop and Destroy each read Proxmox through the provisioner's observer
// and refuse on what they see: Start wants the guest stopped, unlocked and
// unprotected, Stop wants it running, Destroy wants it stopped with the
// configuration digest the approval named. Clone was the one lifecycle step
// that observed nothing at all. It checked the reservation row and the
// template fields it carries and went straight to apply, which writes the plan
// evidence and consumes BeginRunnerVMTerraformApply, the one-way gate that
// makes an apply legal.
//
// So a VMID already occupied by a guest that is not this reservation, a leaked
// runner the reaper never destroyed, a hand-built guest in the disposable
// range, or the previous reservation for the same slot, was found by Terraform
// rather than by this package. By then the intent is spent and the reservation
// is left in the unknown-outcome state that never authorizes a retry, and the
// operator reads a Terraform error about a VMID in use rather than a refusal
// naming the reservation that holds it.
//
// Two answers are accepted:
//
//   - ErrNotFound, the ordinary case: nothing occupies the VMID and the clone
//     is free to create it.
//   - a successful observation, which means the guest at that VMID carries
//     this reservation's own name and marker, because that is the only thing
//     Client.Get returns a VM for. That is a clone that already landed and is
//     being replayed after a crash, and the apply reconciles it.
//
// Everything else refuses, including an observation that simply failed. This
// is a check before a one-way gate, and no request has been issued when it
// answers, so the refusal is an ordinary error rather than an unknown outcome:
// nothing durable happened and the caller may decide again later.
func checkCloneTargetIsFree(ctx context.Context,
	observe func(context.Context, proxmox.Identity) (proxmox.VM, error), target proxmox.Identity) error {
	if observe == nil {
		return errors.New("clone requires an independent observation of its target VMID")
	}
	_, err := observe(ctx, target)
	if err == nil || errors.Is(err, proxmox.ErrNotFound) {
		return nil
	}
	return fmt.Errorf("clone target VMID %d is not free for reservation %s: %w",
		target.VMID, target.ReservationID, err)
}
