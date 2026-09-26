// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
)

func idleProofIdentity() proxmox.Identity {
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

func freshIdleProof(identity proxmox.Identity) proxmox.IdleProof {
	return proxmox.IdleProof{
		VMID:          identity.VMID,
		ReservationID: identity.ReservationID,
		EvidenceID:    "019235f1-0a2b-7c3d-9e4f-5a6b7c8d9e0f",
		ObservedAt:    time.Now().Add(-time.Second),
		Drained:       true,
		NoActiveJob:   true,
	}
}

func TestFreshCooperativeDrainEvidenceIsAccepted(t *testing.T) {
	identity := idleProofIdentity()
	if !validTerraformIdleProof(identity, freshIdleProof(identity)) {
		t.Fatal("refused drain evidence observed one second ago")
	}
}

func TestDrainEvidenceIsRefusedForEveryWayItCanBeWrong(t *testing.T) {
	identity := idleProofIdentity()
	tests := []struct {
		name  string
		spoil func(*proxmox.IdleProof)
	}{
		{name: "another VM", spoil: func(p *proxmox.IdleProof) { p.VMID = 9001 }},
		{name: "another reservation", spoil: func(p *proxmox.IdleProof) {
			p.ReservationID = "019235f1-0a2b-7c3d-9e4f-5a6b7c8d9e0e"
		}},
		{name: "no evidence row", spoil: func(p *proxmox.IdleProof) { p.EvidenceID = "" }},
		{name: "evidence row is not an ID", spoil: func(p *proxmox.IdleProof) { p.EvidenceID = "drain-7" }},
		{name: "not drained", spoil: func(p *proxmox.IdleProof) { p.Drained = false }},
		{name: "a job is still assigned", spoil: func(p *proxmox.IdleProof) { p.NoActiveJob = false }},
		{name: "never observed", spoil: func(p *proxmox.IdleProof) { p.ObservedAt = time.Time{} }},
		{name: "observed in the future", spoil: func(p *proxmox.IdleProof) {
			p.ObservedAt = time.Now().Add(time.Minute)
		}},
		{name: "observed too long ago", spoil: func(p *proxmox.IdleProof) {
			p.ObservedAt = time.Now().Add(-11 * time.Second)
		}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			proof := freshIdleProof(identity)
			test.spoil(&proof)
			if validTerraformIdleProof(identity, proof) {
				t.Fatalf("accepted drain evidence: %s", test.name)
			}
		})
	}
}

func TestStopAndDestroyJudgeDrainEvidenceAlike(t *testing.T) {
	// Stop used to restate the rule inline, in terms identical to this helper
	// down to the second. The two doors read the same evidence and must keep
	// giving it the same answer, which is why there is only one rule left.
	identity := idleProofIdentity()
	destroyProof := proxmox.DestroyProof{
		IdleProof:            freshIdleProof(identity),
		ApprovalID:           "019235f1-0a2b-7c3d-8e4f-5a6b7c8d9e01",
		ExpectedConfigDigest: "0123456789abcdef0123456789abcdef01234567",
		RunnerDeregistered:   true,
		StateReconciled:      true,
	}
	if !validTerraformIdleProof(identity, destroyProof.IdleProof) {
		t.Fatal("the shared rule refused the evidence a destroy carries")
	}
	stale := destroyProof.IdleProof
	stale.ObservedAt = time.Now().Add(-time.Hour)
	if validTerraformIdleProof(identity, stale) {
		t.Fatal("the shared rule accepted hour-old drain evidence")
	}
}
