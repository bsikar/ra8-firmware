// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

// Terraform state is encrypted at rest and locked per reservation, and three
// pure helpers decide what those two things are bound to: the additional data
// a sealed state is authenticated with, the key a state row is locked under,
// and the lock info a client is allowed to claim. None of them needs a
// database, and none of them was covered.

// The additional authenticated data names the reservation, so a ciphertext
// lifted from one machine's row cannot be opened as another's even with the
// same key. Distinct reservations must never share it, and the prefix is what
// keeps it from colliding with any other sealed thing in the plane.
func TestSealedStateIsBoundToItsOwnReservation(t *testing.T) {
	mine := terraformStateAAD(runnerVMTestEvidence)
	theirs := terraformStateAAD(runnerVMTestApproval)

	if string(mine) == string(theirs) {
		t.Fatal("two reservations seal their state under the same authenticated data")
	}
	if !strings.HasPrefix(string(mine), "ra8ci-runner-terraform-state:") {
		t.Fatalf("sealed state is authenticated under %q, which names no purpose", mine)
	}
	if !strings.HasSuffix(string(mine), runnerVMTestEvidence) {
		t.Fatalf("sealed state is authenticated under %q, which does not name its reservation", mine)
	}

	// Rebuilding it is what opening a row does, so it has to be stable.
	if string(terraformStateAAD(runnerVMTestEvidence)) != string(mine) {
		t.Fatal("the same reservation derived two different authenticated data")
	}

	// An empty reservation still derives the prefix rather than an empty
	// string, so a row written without one is not openable as any other row.
	if got := string(terraformStateAAD("")); got != "ra8ci-runner-terraform-state:" {
		t.Fatalf("an unnamed reservation derived %q", got)
	}
}

// The advisory lock is taken on a name derived from the reservation, so two
// machines never serialize against each other and one machine's writers all
// serialize against the same name.
func TestAStateRowLocksUnderItsOwnName(t *testing.T) {
	mine := lockStateKey(runnerVMTestEvidence)
	if mine != "runner-vm-terraform-state:"+runnerVMTestEvidence {
		t.Fatalf("a state row locks under %q", mine)
	}
	if mine == lockStateKey(runnerVMTestApproval) {
		t.Fatal("two machines lock their state rows under the same name")
	}
	if lockStateKey(runnerVMTestEvidence) != mine {
		t.Fatal("the same machine derived two different lock names")
	}

	// The lock name and the seal's authenticated data are deliberately
	// different strings: one is a coordination name that reaches Postgres,
	// the other is cryptographic context that must never be guessable from
	// it. A shared prefix would let a change to one quietly follow the other.
	if mine == string(terraformStateAAD(runnerVMTestEvidence)) {
		t.Fatal("the lock name and the seal's authenticated data are the same string")
	}
}

func aLock() TerraformStateLock {
	return TerraformStateLock{
		ID: runnerVMTestEvidence, Operation: "OperationTypeApply",
		Info: "", Who: "bsikar@lab-1", Version: "1.9.8", Created: "2026-09-29T21:00:00Z",
		Path: "runner-vm/terraform.tfstate",
	}
}

func lockJSON(t *testing.T, lock TerraformStateLock) []byte {
	t.Helper()
	raw, err := json.Marshal(lock)
	if err != nil {
		t.Fatalf("marshal lock: %v", err)
	}
	return raw
}

// Lock info arrives from a Terraform client, so it is read as a claim rather
// than a fact: the ID has to be a real identifier and every free-text field
// is bounded before any of it is written down.
func TestLockInfoIsReadAsAClaim(t *testing.T) {
	lock, err := lockFromJSON(lockJSON(t, aLock()))
	if err != nil {
		t.Fatalf("a whole lock was refused: %v", err)
	}
	if lock.ID != runnerVMTestEvidence || lock.Operation != "OperationTypeApply" || lock.Who != "bsikar@lab-1" {
		t.Fatalf("a lock was read back as %+v", lock)
	}

	atTheBounds := aLock()
	atTheBounds.Operation = strings.Repeat("o", 128)
	atTheBounds.Who = strings.Repeat("w", 256)
	atTheBounds.Version = strings.Repeat("v", 64)
	atTheBounds.Path = strings.Repeat("p", 1024)
	if _, err := lockFromJSON(lockJSON(t, atTheBounds)); err != nil {
		t.Fatalf("a lock exactly at its bounds was refused: %v", err)
	}

	for label, bend := range map[string]func(*TerraformStateLock){
		"no ID":        func(l *TerraformStateLock) { l.ID = "" },
		"an ID by eye": func(l *TerraformStateLock) { l.ID = "lock-1" },
		"an ID of another shape": func(l *TerraformStateLock) {
			l.ID = "01996f90-3415-4cfe-8ff1-600058131aff"
		},
		"no operation":          func(l *TerraformStateLock) { l.Operation = "" },
		"an operation one over": func(l *TerraformStateLock) { l.Operation = strings.Repeat("o", 129) },
		"a holder one over":     func(l *TerraformStateLock) { l.Who = strings.Repeat("w", 257) },
		"a version one over":    func(l *TerraformStateLock) { l.Version = strings.Repeat("v", 65) },
		"a path one over":       func(l *TerraformStateLock) { l.Path = strings.Repeat("p", 1025) },
	} {
		claimed := aLock()
		bend(&claimed)
		if _, err := lockFromJSON(lockJSON(t, claimed)); !errors.Is(err, ErrInvalid) {
			t.Fatalf("a lock with %s was taken: %v", label, err)
		}
	}
}

// Everything a lock is missing is optional except the two that identify it,
// so a minimal client that sends only an ID and an operation still locks.
func TestAMinimalLockStillLocks(t *testing.T) {
	minimal := TerraformStateLock{ID: runnerVMTestEvidence, Operation: "OperationTypeApply"}
	read, err := lockFromJSON(lockJSON(t, minimal))
	if err != nil {
		t.Fatalf("a minimal lock was refused: %v", err)
	}
	if read.Who != "" || read.Path != "" || read.Version != "" {
		t.Fatalf("a minimal lock was filled in as %+v", read)
	}
}

// The body itself is bounded before it is parsed, so a client cannot spend
// the plane's memory on the way to being refused, and a body that is not a
// lock at all is refused rather than read as an empty one.
func TestLockInfoIsBoundedBeforeItIsRead(t *testing.T) {
	for label, raw := range map[string][]byte{
		"nothing at all":     nil,
		"an empty body":      {},
		"a body over 32 KiB": []byte(`{"ID":"` + runnerVMTestEvidence + `","Operation":"apply","Info":"` + strings.Repeat("i", 32<<10) + `"}`),
		"a list":             []byte(`[]`),
		"a word":             []byte(`"locked"`),
		"a bare null":        []byte(`null`),
		"half a lock":        []byte(`{"ID":`),
	} {
		if _, err := lockFromJSON(raw); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s was taken as lock info: %v", label, err)
		}
	}

	// Exactly at the bound is still read, so the refusal above is the size
	// rather than the contents.
	lock := aLock()
	lock.Info = ""
	raw := lockJSON(t, lock)
	lock.Info = strings.Repeat("i", 32<<10-len(raw)-len(`,"Info":""`)+len(`"Info":"",`))
	if atBound := lockJSON(t, lock); len(atBound) <= 32<<10 {
		if _, err := lockFromJSON(atBound); err != nil {
			t.Fatalf("a lock of %d bytes was refused: %v", len(atBound), err)
		}
	}
}

// A state envelope is bounded and its header is read strictly: version 4, a
// lineage that is a UUID, a Terraform version that is stated and bounded, and
// a serial that never runs backwards. The digest handed back is over the body
// as submitted, which is what the reconciliation proof is later bound to.
func TestAStateEnvelopeIsReadStrictly(t *testing.T) {
	body := []byte(`{"version":4,"terraform_version":"1.9.8","serial":3,"lineage":"7b8f6a20-3b0e-4c0f-9a3d-0f9c2b1d4e55"}`)
	header, digest, err := parseTerraformState(body)
	if err != nil {
		t.Fatalf("a whole state envelope was refused: %v", err)
	}
	if header.Serial != 3 || header.TerraformVersion != "1.9.8" {
		t.Fatalf("a state envelope was read back as %+v", header)
	}
	if len(digest) != 64 {
		t.Fatalf("a state digest was %q", digest)
	}

	// The digest is over the bytes as submitted, so the same state written
	// with different spacing is a different digest rather than the same one.
	spaced := []byte(`{"version":4, "terraform_version":"1.9.8", "serial":3, "lineage":"7b8f6a20-3b0e-4c0f-9a3d-0f9c2b1d4e55"}`)
	if _, other, err := parseTerraformState(spaced); err != nil || other == digest {
		t.Fatalf("a respaced state shared the digest %q (err %v)", other, err)
	}

	for label, raw := range map[string]string{
		"no body":                  ``,
		"another state version":    `{"version":3,"terraform_version":"1.9.8","serial":3,"lineage":"7b8f6a20-3b0e-4c0f-9a3d-0f9c2b1d4e55"}`,
		"a serial below zero":      `{"version":4,"terraform_version":"1.9.8","serial":-1,"lineage":"7b8f6a20-3b0e-4c0f-9a3d-0f9c2b1d4e55"}`,
		"no lineage":               `{"version":4,"terraform_version":"1.9.8","serial":3,"lineage":""}`,
		"a lineage by eye":         `{"version":4,"terraform_version":"1.9.8","serial":3,"lineage":"lineage-1"}`,
		"a shouted lineage":        `{"version":4,"terraform_version":"1.9.8","serial":3,"lineage":"7B8F6A20-3B0E-4C0F-9A3D-0F9C2B1D4E55"}`,
		"no Terraform version":     `{"version":4,"terraform_version":"","serial":3,"lineage":"7b8f6a20-3b0e-4c0f-9a3d-0f9c2b1d4e55"}`,
		"a Terraform version long": `{"version":4,"terraform_version":"` + strings.Repeat("9", 65) + `","serial":3,"lineage":"7b8f6a20-3b0e-4c0f-9a3d-0f9c2b1d4e55"}`,
		"half an envelope":         `{"version":4,`,
		"a list":                   `[]`,
	} {
		if _, _, err := parseTerraformState([]byte(raw)); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s was taken as a state envelope: %v", label, err)
		}
	}
}
