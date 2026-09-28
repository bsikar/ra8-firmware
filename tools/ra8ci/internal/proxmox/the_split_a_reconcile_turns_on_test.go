// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"sync"
	"testing"
)

// Once a request has actually been sent, the question stops being "did it
// work" and becomes "does somebody have to go and look". A refusal the client
// can be sure of is a plain error; anything it cannot be sure of has to come
// back as an unknown outcome carrying the operation ID, because that is what a
// later reconcile is filed under. Getting that split wrong in either direction
// is expensive: a false plain error strands a guest nobody reconciles, and a
// false unknown outcome sends an operator after a request that never landed.

// deletingLab serves a destroy end to end and lets the verification that
// follows the task be bent: the listing can fail on a later read, and the
// guest can be left in it as though the delete never took.
type deletingLab struct {
	mu            sync.Mutex
	listings      int
	failListingAt int
	keepGuest     bool
	deleted       bool
}

func (l *deletingLab) serve(w http.ResponseWriter, r *http.Request) {
	l.mu.Lock()
	defer l.mu.Unlock()
	switch {
	case r.Method == http.MethodDelete:
		l.deleted = true
		w.Header().Set("Content-Type", jsonType)
		_, _ = w.Write([]byte(`{"data":"` + fakeUPID("qmdestroy") + `"}`))
	case r.URL.Path == "/api2/json/cluster/resources":
		l.listings++
		if l.failListingAt == l.listings {
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", jsonType)
		if l.deleted && !l.keepGuest {
			_, _ = w.Write([]byte(`{"data":[]}`))
			return
		}
		_, _ = w.Write([]byte(`{"data":[` + guestRecord(9000, testIdentity.Name) + `]}`))
	case strings.HasSuffix(r.URL.Path, "/status"):
		upid := strings.TrimSuffix(strings.TrimPrefix(r.URL.Path, "/api2/json/nodes/pve/tasks/"), "/status")
		w.Header().Set("Content-Type", jsonType)
		_, _ = w.Write([]byte(`{"data":{"upid":"` + upid + `","node":"pve","id":"9000","type":"qmdestroy","status":"stopped","exitstatus":"OK"}}`))
	default:
		reservedLab(reservedConfig(), "stopped", 0)(w, r)
	}
}

// A mutation the cluster refused outright is a plain error. The request did
// not land, so there is nothing to reconcile and nobody should be sent looking.
func TestAMutationTheClusterRefusedIsNotAnUnknownOutcome(t *testing.T) {
	for _, status := range []int{http.StatusBadRequest, http.StatusUnauthorized, http.StatusForbidden, http.StatusNotFound} {
		t.Run(http.StatusText(status), func(t *testing.T) {
			f := newFake()
			f.exists = true
			f.status = "running"
			f.failMutationStatus = status
			client, _ := testClient(t, f)

			_, err := client.Stop(context.Background(), Action{ID: testAction}, testIdentity, idleProof())
			if err == nil {
				t.Fatal("a refused stop was reported as done")
			}
			var unknown *UnknownOutcomeError
			if errors.As(err, &unknown) {
				t.Fatalf("a refusal the cluster was explicit about became an unknown outcome: %v", err)
			}
		})
	}
}

// Everything after the request has been sent is unknown until it is verified,
// and every one of these has to name the operation so a reconcile can be filed.
func TestWhatIsNotVerifiedComesBackAsAnUnknownOutcome(t *testing.T) {
	t.Run("a task that did not finish OK", func(t *testing.T) {
		f := newFake()
		f.exists = true
		f.status = "running"
		f.taskOverride = map[string]any{"exitstatus": "command failed"}
		client, _ := testClient(t, f)

		_, err := client.Stop(context.Background(), Action{ID: testAction}, testIdentity, idleProof())
		var unknown *UnknownOutcomeError
		if !errors.As(err, &unknown) {
			t.Fatalf("error = %v, want an unknown outcome", err)
		}
		if unknown.OperationID != testAction || unknown.UPID == "" {
			t.Fatalf("the unknown outcome named neither the operation nor the task: %+v", unknown)
		}
	})

	t.Run("a guest still listed after its delete", func(t *testing.T) {
		lab := &deletingLab{keepGuest: true}
		client := clientAnswering(t, lab.serve)

		_, err := client.Destroy(context.Background(), Action{ID: testAction}, testIdentity, reviewedCleanup())
		var unknown *UnknownOutcomeError
		if !errors.As(err, &unknown) {
			t.Fatalf("error = %v, want an unknown outcome", err)
		}
		if !errors.Is(unknown.Cause, ErrConflict) || unknown.UPID == "" {
			t.Fatalf("a guest that survived its delete was not reported as a conflict on a known task: %v", err)
		}
	})

	t.Run("a delete whose result cannot be read back", func(t *testing.T) {
		lab := &deletingLab{failListingAt: 2}
		client := clientAnswering(t, lab.serve)

		_, err := client.Destroy(context.Background(), Action{ID: testAction}, testIdentity, reviewedCleanup())
		var unknown *UnknownOutcomeError
		if !errors.As(err, &unknown) {
			t.Fatalf("error = %v, want an unknown outcome", err)
		}
		if unknown.OperationID != testAction {
			t.Fatalf("the unknown outcome named operation %q", unknown.OperationID)
		}
	})

	t.Run("a delete that did take", func(t *testing.T) {
		lab := &deletingLab{}
		client := clientAnswering(t, lab.serve)

		result, err := client.Destroy(context.Background(), Action{ID: testAction}, testIdentity, reviewedCleanup())
		if err != nil {
			t.Fatalf("a delete the listing confirmed was still refused: %v", err)
		}
		if result.UPID == "" || result.VM != nil {
			t.Fatalf("a completed delete answered %+v, want the task ID and no guest", result)
		}
	})
}

// A reconcile observes an operation the caller could not verify. It never
// re-issues anything, so what it reports is only ever what it could read.
func TestAReconcileReportsOnlyWhatItCouldRead(t *testing.T) {
	t.Run("a task that failed", func(t *testing.T) {
		f := newFake()
		f.exists = true
		f.taskOverride = map[string]any{"exitstatus": "command failed"}
		client, _ := testClient(t, f)

		upid := fakeUPID("qmstop")
		_, err := client.Reconcile(context.Background(), testAction, testIdentity, "stop", upid)
		var unknown *UnknownOutcomeError
		if !errors.As(err, &unknown) {
			t.Fatalf("error = %v, want an unknown outcome", err)
		}
		if unknown.UPID != upid {
			t.Fatalf("the unknown outcome named task %q, want %q", unknown.UPID, upid)
		}
	})

	t.Run("a task whose status the caller stopped waiting for", func(t *testing.T) {
		f := newFake()
		f.exists = true
		f.taskRunning = true
		client, _ := testClient(t, f)
		ctx, cancel := context.WithCancel(context.Background())
		cancel()

		upid := fakeUPID("qmstop")
		_, err := client.Reconcile(ctx, testAction, testIdentity, "stop", upid)
		var unknown *UnknownOutcomeError
		if !errors.As(err, &unknown) {
			t.Fatalf("error = %v, want an unknown outcome", err)
		}
		if unknown.UPID != upid || !strings.Contains(err.Error(), "context canceled") {
			t.Fatalf("a wait the caller abandoned did not name the task it abandoned: %v", err)
		}
	})

	t.Run("a lost clone over a guest that is not ours", func(t *testing.T) {
		f := newFake()
		f.exists = true
		f.pool = "other"
		client, _ := testClient(t, f)

		_, err := client.Reconcile(context.Background(), testIdentity.CreationOperationID, testIdentity, "clone", "")
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("error = %v, want the conflict carried back as itself", err)
		}
		var unknown *UnknownOutcomeError
		if errors.As(err, &unknown) {
			t.Fatalf("a guest the client could read, and refused, became an unknown outcome: %v", err)
		}
	})
}
