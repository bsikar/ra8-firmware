// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Every lifecycle command (clone, start, stop, destroy) reaches the lab
// through reservation(), which is where a caller's claimed identity is held
// against the durable ledger row and the reviewed profile. It answers three
// different refusals, and which one a caller gets tells them what to do next:
// ErrInvalid means the identity was never admissible, ErrConflict means the
// ledger disagrees with what the caller believes it holds, and the profile
// refusal means the durable row has drifted from the reviewed policy.

const (
	tfReservationID = "0192f3a4-b5c6-7d8e-9f01-1234567890ab"
	tfCreationID    = "0192f3a4-b5c6-7d8e-9f01-1234567890ac"
)

// reservationLedger answers one durable row and counts the reads, so a test
// can tell a refusal that never consulted the ledger from one that did.
type reservationLedger struct {
	policyLedger
	vm    store.RunnerVM
	err   error
	reads int
}

func (l *reservationLedger) GetRunnerVM(context.Context, string) (store.RunnerVM, error) {
	l.reads++
	return l.vm, l.err
}

func reservedIdentity() proxmox.Identity {
	return proxmox.Identity{
		VMID:                9000,
		Node:                "pve-lab-1",
		Pool:                "ra8-tf-lab",
		Storage:             "ra8-tf-lab",
		Name:                "ra8-lab-ci-9000",
		ReservationID:       tfReservationID,
		CreationOperationID: tfCreationID,
	}
}

func reservedVM(identity proxmox.Identity) store.RunnerVM {
	return store.RunnerVM{
		ID: identity.ReservationID,
		RunnerVMInput: store.RunnerVMInput{
			VMID:         identity.VMID,
			Node:         identity.Node,
			Pool:         identity.Pool,
			Storage:      identity.Storage,
			Name:         identity.Name,
			TemplateVMID: 9001,
		},
		CreationOperationID: identity.CreationOperationID,
	}
}

func reservationProvisioner(t *testing.T, ledger *reservationLedger,
	config TerraformRunnerConfig) *TerraformRunnerProvisioner {
	t.Helper()
	keys, err := NewSSHAccessStore(filepath.Join(t.TempDir(), "keys"))
	if err != nil {
		t.Fatalf("prepare SSH access store: %v", err)
	}
	provisioner, err := NewTerraformRunnerProvisioner(&TerraformRuntime{}, ledger, policyObserver{}, keys, config)
	if err != nil {
		t.Fatalf("reviewed disposable policy refused: %v", err)
	}
	return provisioner
}

// reservedProvisioner is the healthy pairing every refusal below is one edit
// away from: the reviewed 9000 profile, a durable row that agrees with it, and
// an identity that names both.
func reservedProvisioner(t *testing.T) (*TerraformRunnerProvisioner, *reservationLedger) {
	t.Helper()
	identity := reservedIdentity()
	ledger := &reservationLedger{vm: reservedVM(identity)}
	return reservationProvisioner(t, ledger, reviewedConfig(t)), ledger
}

func TestTheAgreedReservationIsReturnedWhole(t *testing.T) {
	provisioner, ledger := reservedProvisioner(t)
	vm, err := provisioner.reservation(context.Background(), reservedIdentity())
	if err != nil {
		t.Fatalf("an agreed reservation must be accepted: %v", err)
	}
	if vm.ID != tfReservationID || vm.VMID != 9000 || vm.TemplateVMID != 9001 {
		t.Fatalf("the durable row must be returned as it stands, got %+v", vm)
	}
	if ledger.reads != 1 {
		t.Fatalf("the ledger must be read exactly once, got %d", ledger.reads)
	}
}

// The identity is judged before the ledger is touched. A caller who never had
// an admissible identity learns that without a database round trip, and a
// malformed reservation ID never reaches a query.
func TestAnInadmissibleIdentityNeverReachesTheLedger(t *testing.T) {
	for name, edit := range map[string]func(*proxmox.Identity){
		"name does not follow its VMID": func(i *proxmox.Identity) { i.Name = "ra8-lab-ci-9001" },
		"empty name":                    func(i *proxmox.Identity) { i.Name = "" },
		"node outside the profile":      func(i *proxmox.Identity) { i.Node = "pve-lab-2" },
		"pool outside the profile":      func(i *proxmox.Identity) { i.Pool = "ra8-other" },
		"storage outside the profile":   func(i *proxmox.Identity) { i.Storage = "local-lvm" },
		"unknown VMID":                  func(i *proxmox.Identity) { i.VMID = 9042; i.Name = "ra8-lab-ci-9042" },
		"malformed reservation ID":      func(i *proxmox.Identity) { i.ReservationID = "not-an-identifier" },
		"empty reservation ID":          func(i *proxmox.Identity) { i.ReservationID = "" },
		"malformed creation operation":  func(i *proxmox.Identity) { i.CreationOperationID = "not-an-identifier" },
		"empty creation operation":      func(i *proxmox.Identity) { i.CreationOperationID = "" },
	} {
		provisioner, ledger := reservedProvisioner(t)
		identity := reservedIdentity()
		edit(&identity)
		_, err := provisioner.reservation(context.Background(), identity)
		if !errors.Is(err, proxmox.ErrInvalid) {
			t.Fatalf("%s must be refused as invalid, got %v", name, err)
		}
		if ledger.reads != 0 {
			t.Fatalf("%s must be refused before the ledger is read", name)
		}
	}
}

// The disposable window is checked on its own rather than being left to the
// profile map, so a profile filed under a VMID outside 9000-9099 cannot pull a
// guest onto an ID the lab reserves for something else.
func TestTheDisposableWindowIsCheckedBesideTheProfileMap(t *testing.T) {
	for _, vmid := range []int{8999, 9100} {
		config := reviewedConfig(t)
		config.Profiles = map[int]TerraformRunnerProfile{vmid: reviewedProfile()}
		identity := reservedIdentity()
		identity.VMID = vmid
		identity.Name = fmt.Sprintf("ra8-lab-ci-%d", vmid)
		ledger := &reservationLedger{vm: reservedVM(identity)}
		provisioner, err := NewTerraformRunnerProvisioner(&TerraformRuntime{}, ledger, policyObserver{},
			mustKeys(t), config)
		if err != nil {
			// A profile outside the window may be refused at the door
			// instead, which is the same answer one step earlier.
			continue
		}
		if _, err := provisioner.reservation(context.Background(), identity); !errors.Is(err, proxmox.ErrInvalid) {
			t.Fatalf("VMID %d sits outside the disposable window and must be refused, got %v", vmid, err)
		}
		if ledger.reads != 0 {
			t.Fatalf("VMID %d must be refused before the ledger is read", vmid)
		}
	}
	// Both ends of the window are admissible identities.
	for _, vmid := range []int{9000, 9099} {
		config := reviewedConfig(t)
		config.Profiles = map[int]TerraformRunnerProfile{vmid: reviewedProfile()}
		identity := reservedIdentity()
		identity.VMID = vmid
		identity.Name = fmt.Sprintf("ra8-lab-ci-%d", vmid)
		ledger := &reservationLedger{vm: reservedVM(identity)}
		provisioner := reservationProvisioner(t, ledger, config)
		if _, err := provisioner.reservation(context.Background(), identity); err != nil {
			t.Fatalf("VMID %d sits inside the disposable window and must be admitted, got %v", vmid, err)
		}
	}
}

func mustKeys(t *testing.T) *SSHAccessStore {
	t.Helper()
	keys, err := NewSSHAccessStore(filepath.Join(t.TempDir(), "keys"))
	if err != nil {
		t.Fatalf("prepare SSH access store: %v", err)
	}
	return keys
}

// A missing context is refused like a malformed identity rather than being
// filled in, because the deadline a caller carries is what bounds every
// Proxmox and Terraform call past this point.
func TestAMissingContextIsRefusedAndANilProvisionerDoesNotPanic(t *testing.T) {
	provisioner, ledger := reservedProvisioner(t)
	//nolint:staticcheck // a nil context is exactly what this guard exists for
	if _, err := provisioner.reservation(nil, reservedIdentity()); !errors.Is(err, proxmox.ErrInvalid) {
		t.Fatalf("a missing context must be refused as invalid, got %v", err)
	}
	if ledger.reads != 0 {
		t.Fatal("a missing context must be refused before the ledger is read")
	}

	var absent *TerraformRunnerProvisioner
	if _, err := absent.reservation(context.Background(), reservedIdentity()); !errors.Is(err, proxmox.ErrInvalid) {
		t.Fatalf("a nil provisioner must answer invalid rather than panic, got %v", err)
	}
}

// The ledger's own verdict is passed through untouched: a reservation that is
// simply not there must not be reported as a conflict, because the two lead a
// caller to different next steps.
func TestTheLedgerVerdictIsPassedThroughUnchanged(t *testing.T) {
	for _, want := range []error{store.ErrNotFound, store.ErrConflict, errors.New("database unavailable")} {
		identity := reservedIdentity()
		ledger := &reservationLedger{vm: reservedVM(identity), err: want}
		provisioner := reservationProvisioner(t, ledger, reviewedConfig(t))
		_, err := provisioner.reservation(context.Background(), identity)
		if !errors.Is(err, want) {
			t.Fatalf("the ledger's verdict must stand, want %v got %v", want, err)
		}
	}
}

// Every field the caller claims is held against the durable row, and a
// disagreement in any of them is a conflict rather than a silent correction:
// the row is the record of what was actually reserved.
func TestEveryClaimedFieldIsHeldAgainstTheDurableRow(t *testing.T) {
	for name, edit := range map[string]func(*store.RunnerVM){
		"another reservation":        func(vm *store.RunnerVM) { vm.ID = tfCreationID },
		"another VMID":               func(vm *store.RunnerVM) { vm.VMID = 9001 },
		"another node":               func(vm *store.RunnerVM) { vm.Node = "pve-lab-2" },
		"another pool":               func(vm *store.RunnerVM) { vm.Pool = "ra8-other" },
		"another datastore":          func(vm *store.RunnerVM) { vm.Storage = "local-lvm" },
		"another name":               func(vm *store.RunnerVM) { vm.Name = "ra8-lab-ci-9099" },
		"another creation operation": func(vm *store.RunnerVM) { vm.CreationOperationID = tfReservationID },
		"an empty row":               func(vm *store.RunnerVM) { *vm = store.RunnerVM{} },
	} {
		identity := reservedIdentity()
		vm := reservedVM(identity)
		edit(&vm)
		ledger := &reservationLedger{vm: vm}
		provisioner := reservationProvisioner(t, ledger, reviewedConfig(t))
		_, err := provisioner.reservation(context.Background(), identity)
		if !errors.Is(err, proxmox.ErrConflict) {
			t.Fatalf("a row naming %s must be a conflict, got %v", name, err)
		}
	}
}

// The profile comparison at the end has exactly one reachable refusal, and it
// is worth knowing which. Node, pool and datastore are already bound: the
// identity had to match the profile to be admissible, and the row had to match
// the identity to avoid the conflict above. The template is the one field
// nothing earlier constrains, so a row cloned from a template the reviewed
// profile no longer names is the drift this block actually catches.
func TestADurableRowFromAnotherTemplateIsRefusedAgainstTheProfile(t *testing.T) {
	identity := reservedIdentity()
	vm := reservedVM(identity)
	vm.TemplateVMID = 9002
	ledger := &reservationLedger{vm: vm}
	provisioner := reservationProvisioner(t, ledger, reviewedConfig(t))

	_, err := provisioner.reservation(context.Background(), identity)
	if err == nil {
		t.Fatal("a row cloned from an unreviewed template must be refused")
	}
	if errors.Is(err, proxmox.ErrConflict) || errors.Is(err, proxmox.ErrInvalid) {
		t.Fatalf("template drift must not be reported as an identity conflict, got %v", err)
	}
	if !strings.Contains(err.Error(), "fixed Terraform profile") {
		t.Fatalf("the refusal must name the fixed profile, got %v", err)
	}

	// The same row against a profile that does name that template is fine, so
	// the refusal is about the disagreement and not about the template ID.
	config := reviewedConfig(t)
	profile := reviewedProfile()
	profile.TemplateVMID = 9002
	config.Profiles = map[int]TerraformRunnerProfile{9000: profile}
	agreed := reservationProvisioner(t, &reservationLedger{vm: vm}, config)
	if _, err := agreed.reservation(context.Background(), identity); err != nil {
		t.Fatalf("a row matching its reviewed template must be accepted: %v", err)
	}
}

// The policy the row is held against is the provisioner's own copy, taken at
// admission. An operator editing the map they passed in afterwards cannot
// retarget a live reservation at another template.
func TestTheReservationIsHeldAgainstTheAdmittedCopyOfThePolicy(t *testing.T) {
	config := reviewedConfig(t)
	identity := reservedIdentity()
	ledger := &reservationLedger{vm: reservedVM(identity)}
	provisioner := reservationProvisioner(t, ledger, config)

	drifted := reviewedProfile()
	drifted.TemplateVMID = 9002
	config.Profiles[9000] = drifted

	if _, err := provisioner.reservation(context.Background(), identity); err != nil {
		t.Fatalf("an edit to the caller's map must not reach the admitted policy: %v", err)
	}
}
