// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"encoding/pem"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// observedAgo is a proof this client would accept at the door, aged.
func observedAgo(age time.Duration) IdleProof {
	proof := idleProof()
	proof.ObservedAt = time.Now().Add(-age)
	return proof
}

// slowPVE answers exactly as the package fake does, after a delay on every
// request, so a reservation read spends real time before a mutation is issued.
func slowPVE(t *testing.T, f *fakePVE, delay time.Duration) *Client {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(delay)
		f.serve(w, r)
	}))
	t.Cleanup(server.Close)
	cert := server.Certificate()
	if cert == nil {
		t.Fatal("test server has no certificate")
	}
	dir := t.TempDir()
	ca := filepath.Join(dir, "ca.pem")
	if err := os.WriteFile(ca, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: cert.Raw}), 0600); err != nil {
		t.Fatal(err)
	}
	token := filepath.Join(dir, "token")
	if err := os.WriteFile(token, []byte("ra8ci@pve!client=secret-token\n"), 0600); err != nil {
		t.Fatal(err)
	}
	client, err := New(Config{
		Endpoint: server.URL, CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8", "vmbr9"},
		RequestTimeout: 5 * time.Second, OperationTimeout: 20 * time.Second, TaskPollInterval: time.Millisecond,
	})
	if err != nil {
		t.Fatal(err)
	}
	return client
}

func mutations(t *testing.T, f *fakePVE) []string {
	t.Helper()
	f.mu.Lock()
	defer f.mu.Unlock()
	var issued []string
	for _, request := range f.requests {
		if !strings.HasPrefix(request, "GET ") {
			issued = append(issued, request)
		}
	}
	return issued
}

// The door and the mutation must apply ONE window, not two that can drift.
func TestTheReCheckAppliesExactlyTheWindowTheDoorApplies(t *testing.T) {
	now := time.Now()
	for _, age := range []time.Duration{
		0,
		time.Millisecond,
		idleProofFreshness / 2,
		idleProofFreshness - time.Millisecond,
		idleProofFreshness,
		idleProofFreshness + time.Millisecond,
		time.Minute,
		time.Hour,
	} {
		proof := IdleProof{
			VMID: testIdentity.VMID, ReservationID: testIdentity.ReservationID, EvidenceID: testEvidence,
			ObservedAt: now.Add(-age), Drained: true, NoActiveJob: true,
		}
		atTheDoor := validateIdleProof(testIdentity, proof, now) == nil
		atTheMutation := checkIdleProofStillFresh(proof, now) == nil
		if atTheDoor != atTheMutation {
			t.Fatalf("age %s: door accepts %v, mutation accepts %v", age, atTheDoor, atTheMutation)
		}
	}
}

// The bound is inclusive and was inclusive before: a proof exactly at the
// window is still acted on, so the re-check tightens nothing by accident.
func TestAProofExactlyAtTheWindowIsStillActedOn(t *testing.T) {
	now := time.Now()
	proof := IdleProof{ObservedAt: now.Add(-idleProofFreshness)}
	if !idleProofIsFresh(proof, now) {
		t.Fatal("a proof exactly at the window was refused")
	}
	if idleProofIsFresh(IdleProof{ObservedAt: now.Add(-idleProofFreshness - time.Nanosecond)}, now) {
		t.Fatal("a proof past the window was accepted")
	}
}

func TestAnUnobservedProofIsNeverFresh(t *testing.T) {
	if idleProofIsFresh(IdleProof{}, time.Now()) {
		t.Fatal("a proof that was never observed read as fresh")
	}
}

// Evidence from the future is refused past the skew allowance rather than
// read as very recent; inside the allowance it is a clock difference.
func TestEvidenceFromTheFutureIsRefusedPastTheSkewAllowance(t *testing.T) {
	now := time.Now()
	if !idleProofIsFresh(IdleProof{ObservedAt: now.Add(idleProofSkew)}, now) {
		t.Fatal("a small clock difference was refused")
	}
	if idleProofIsFresh(IdleProof{ObservedAt: now.Add(idleProofSkew + time.Second)}, now) {
		t.Fatal("evidence from the future was read as fresh")
	}
}

// Going stale mid-operation is its own fact, and still an invalid input, so a
// caller that only asks whether the input was refused is unaffected.
func TestStaleEvidenceIsBothInvalidAndNamedAsStale(t *testing.T) {
	err := checkIdleProofStillFresh(observedAgo(time.Minute), time.Now())
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("stale evidence is no longer an invalid input: %v", err)
	}
	if !errors.Is(err, ErrEvidenceStale) {
		t.Fatalf("stale evidence is not distinguishable from the refusal at the door: %v", err)
	}
	if !strings.Contains(err.Error(), "9000") {
		t.Fatalf("refusal does not name the guest: %v", err)
	}
}

// The gap itself: evidence fresh at the door, aged past the window by the
// reads Stop makes before it issues anything.
func TestEvidenceThatAgesDuringTheReservationReadStopsNoGuest(t *testing.T) {
	f := newFake()
	f.exists = true
	f.status = "running"
	client := slowPVE(t, f, 150*time.Millisecond)
	proof := observedAgo(idleProofFreshness - 250*time.Millisecond)
	if err := validateIdleProof(testIdentity, proof, time.Now()); err != nil {
		t.Fatalf("test proof is not fresh at the door: %v", err)
	}
	_, err := client.Stop(context.Background(), Action{ID: testAction}, testIdentity, proof)
	if !errors.Is(err, ErrEvidenceStale) {
		t.Fatalf("hard stop issued on evidence that went stale: %v", err)
	}
	if issued := mutations(t, f); issued != nil {
		t.Fatalf("refused stop still mutated the guest: %v", issued)
	}
}

func TestEvidenceThatAgesDuringTheReservationReadDestroysNoGuest(t *testing.T) {
	f := newFake()
	f.exists = true
	client := slowPVE(t, f, 150*time.Millisecond)
	proof := DestroyProof{
		IdleProof: observedAgo(idleProofFreshness - 250*time.Millisecond), ApprovalID: testApproval,
		ExpectedConfigDigest: testDigest, RunnerDeregistered: true, StateReconciled: true,
	}
	_, err := client.Destroy(context.Background(), Action{ID: testAction}, testIdentity, proof)
	if !errors.Is(err, ErrEvidenceStale) {
		t.Fatalf("guest deleted on evidence that went stale: %v", err)
	}
	if issued := mutations(t, f); issued != nil {
		t.Fatalf("refused destroy still mutated the guest: %v", issued)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if !f.exists {
		t.Fatal("refused destroy removed the guest anyway")
	}
}

// A slow read is not itself a refusal: evidence observed now survives it.
func TestASlowReservationReadStillStopsAGuestOnFreshEvidence(t *testing.T) {
	f := newFake()
	f.exists = true
	f.status = "running"
	client := slowPVE(t, f, 150*time.Millisecond)
	stopped, err := client.Stop(context.Background(), Action{ID: testAction}, testIdentity, idleProof())
	if err != nil || stopped.VM == nil || stopped.VM.Status != "stopped" {
		t.Fatalf("fresh evidence did not stop the guest: %+v, %v", stopped, err)
	}
}

// An already-stopped guest issues nothing, so stale evidence there is not a
// refusal to report: the re-check sits after that branch, deliberately.
func TestAnAlreadyStoppedGuestIsStillReportedSatisfied(t *testing.T) {
	f := newFake()
	f.exists = true
	f.status = "stopped"
	client, _ := testClient(t, f)
	result, err := client.Stop(context.Background(), Action{ID: testAction}, testIdentity, idleProof())
	if err != nil || !result.AlreadySatisfied {
		t.Fatalf("already-stopped guest not reported satisfied: %+v, %v", result, err)
	}
}
