// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// Stop, Destroy and Reconcile all read the guest before they write anything,
// and each of those reads can refuse. What matters is that a refusal costs no
// mutation: a stop or a delete sent against a guest whose state the client
// could not establish is the one mistake that cannot be taken back.

func mutationsSent(f *fakePVE) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	sent := 0
	for _, request := range f.requests {
		if !strings.HasPrefix(request, "GET ") {
			sent++
		}
	}
	return sent
}

func reviewedCleanup() DestroyProof {
	return DestroyProof{IdleProof: idleProof(), ApprovalID: testApproval, ExpectedConfigDigest: testDigest, RunnerDeregistered: true, StateReconciled: true}
}

// An operation ID that is not a canonical UUID is refused by every method that
// takes one, before the guest is read at all. Without it there is nothing to
// tie a lost request back to, which is the whole basis of the reconcile.
func TestAnOperationWithoutACanonicalIDIsRefusedBeforeTheGuestIsRead(t *testing.T) {
	for _, attempt := range []struct {
		name string
		call func(*Client) error
	}{
		{"a stop", func(c *Client) error {
			_, err := c.Stop(context.Background(), Action{ID: "stop-1"}, testIdentity, idleProof())
			return err
		}},
		{"a destroy", func(c *Client) error {
			_, err := c.Destroy(context.Background(), Action{ID: ""}, testIdentity, reviewedCleanup())
			return err
		}},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			f := newFake()
			f.exists = true
			f.status = "running"
			client, _ := testClient(t, f)

			err := attempt.call(client)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("error = %v, want an invalid-input refusal", err)
			}
			if sent := mutationsSent(f); sent != 0 {
				t.Fatalf("a refused operation still sent %d mutations", sent)
			}
			f.mu.Lock()
			reads := len(f.requests)
			f.mu.Unlock()
			if reads != 0 {
				t.Fatalf("a refused operation still read the guest %d times", reads)
			}
		})
	}
}

// A guest the client cannot read is never stopped or deleted. An absent guest
// carries ErrNotFound out as itself, so a caller can tell "gone" from "cannot
// tell"; a locked guest is a conflict, because Proxmox is mid-operation on it
// and a second request would race whatever holds the lock.
func TestAStopAndADestroyRefuseAGuestTheyCannotEstablish(t *testing.T) {
	for _, attempt := range []struct {
		name  string
		setUp func(*fakePVE)
		call  func(*Client) error
		want  error
	}{
		{"a stop over a guest that is not there", func(f *fakePVE) { f.exists = false },
			func(c *Client) error {
				_, err := c.Stop(context.Background(), Action{ID: testAction}, testIdentity, idleProof())
				return err
			}, ErrNotFound},
		{"a stop over a locked guest", func(f *fakePVE) { f.exists = true; f.status = "running"; f.lock = "backup" },
			func(c *Client) error {
				_, err := c.Stop(context.Background(), Action{ID: testAction}, testIdentity, idleProof())
				return err
			}, ErrConflict},
		{"a destroy over a guest that is not there", func(f *fakePVE) { f.exists = false },
			func(c *Client) error {
				_, err := c.Destroy(context.Background(), Action{ID: testAction}, testIdentity, reviewedCleanup())
				return err
			}, ErrNotFound},
		{"a destroy over a locked guest", func(f *fakePVE) { f.exists = true; f.lock = "backup" },
			func(c *Client) error {
				_, err := c.Destroy(context.Background(), Action{ID: testAction}, testIdentity, reviewedCleanup())
				return err
			}, ErrConflict},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			f := newFake()
			attempt.setUp(f)
			client, _ := testClient(t, f)

			err := attempt.call(client)
			if !errors.Is(err, attempt.want) {
				t.Fatalf("error = %v, want %v", err, attempt.want)
			}
			var unknown *UnknownOutcomeError
			if errors.As(err, &unknown) {
				t.Fatalf("a refusal before any request claimed an unknown outcome: %v", err)
			}
			if sent := mutationsSent(f); sent != 0 {
				t.Fatalf("a refused operation still sent %d mutations", sent)
			}
		})
	}
}

// A reconcile exists to observe an operation whose outcome is unknown, so its
// own arguments are held to the same standard as the operation's: a guest
// outside the approvals, an operation ID that is not a UUID, and a kind this
// client never issues are all refused before anything is read.
func TestAReconcileRefusesWhatItCannotObserve(t *testing.T) {
	foreign := testIdentity
	foreign.VMID = 8999

	for _, attempt := range []struct {
		name     string
		identity Identity
		id       string
		kind     string
	}{
		{"a guest outside the approvals", foreign, testAction, "stop"},
		{"an operation ID that is not a UUID", testIdentity, "stop-1", "stop"},
		{"a kind this client never issues", testIdentity, testAction, "migrate"},
		{"no kind at all", testIdentity, testAction, ""},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			f := newFake()
			f.exists = true
			client, _ := testClient(t, f)

			result, err := client.Reconcile(context.Background(), attempt.id, attempt.identity, attempt.kind, fakeUPID("qmstop"))
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("error = %v, want an invalid-input refusal", err)
			}
			if result.VM != nil || result.UPID != "" || result.AlreadySatisfied {
				t.Fatalf("a refused reconcile still answered: %+v", result)
			}
			f.mu.Lock()
			reads := len(f.requests)
			f.mu.Unlock()
			if reads != 0 {
				t.Fatalf("a refused reconcile still spoke to the hypervisor %d times", reads)
			}
		})
	}
}
