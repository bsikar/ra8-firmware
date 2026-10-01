// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"errors"
	"strings"
	"testing"
)

func taskIDFor(kind, guest string) string {
	return "UPID:pve:00000001:00000000:ABCD:" + kind + ":" + guest + ":api@pve!token:"
}

func TestAWellFormedTaskIDParses(t *testing.T) {
	parsed, err := parseTaskID(taskIDFor("qmclone", "9000"), "pve", "clone")
	if err != nil {
		t.Fatalf("canonical task ID refused: %v", err)
	}
	if parsed.Node != "pve" || parsed.Type != "qmclone" || parsed.Guest != "9000" {
		t.Fatalf("task ID read as %+v", parsed)
	}
}

func TestEveryLifecycleKindNamesItsWorkerType(t *testing.T) {
	for kind, want := range map[string]string{"clone": "qmclone", "start": "qmstart", "stop": "qmstop", "destroy": "qmdestroy"} {
		if got := taskTypeFor(kind); got != want {
			t.Fatalf("kind %s named worker type %q, want %q", kind, got, want)
		}
		if _, err := parseTaskID(taskIDFor(want, "9000"), "pve", kind); err != nil {
			t.Fatalf("kind %s refused its own task ID: %v", kind, err)
		}
	}
	if taskTypeFor("bogus") != "" {
		t.Fatal("an unknown kind named a worker type")
	}
}

func TestAnIncompleteTaskIDIsRefused(t *testing.T) {
	for _, upid := range []string{
		"UPID:pve:qmstart",
		"UPID:pve:00000001:00000000:ABCD:qmstart:9000",
		"UPID:pve:00000001:00000000:ABCD:qmstart:9000:api@pve!token",
		"UPID:pve::00000000:ABCD:qmstart:9000:api@pve!token:",
		"UPID:pve:00000001:00000000:ABCD:qmstart::api@pve!token:",
		"not-a-task-id",
		"",
	} {
		if _, err := parseTaskID(upid, "pve", "start"); !errors.Is(err, ErrInvalid) {
			t.Fatalf("task ID %q accepted: %v", upid, err)
		}
	}
}

func TestATaskIDCarryingATrailingCommentIsAccepted(t *testing.T) {
	if _, err := parseTaskID(taskIDFor("qmstop", "9000")+"scheduled.by.ra8ci", "pve", "stop"); err != nil {
		t.Fatalf("task ID with a comment refused: %v", err)
	}
}

func TestATaskIDFromAnotherNodeIsRefused(t *testing.T) {
	_, err := parseTaskID(taskIDFor("qmstart", "9000"), "pve2", "start")
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "node") {
		t.Fatalf("task ID from another node accepted: %v", err)
	}
}

func TestATaskIDForAnotherOperationIsRefused(t *testing.T) {
	_, err := parseTaskID(taskIDFor("qmstop", "9000"), "pve", "start")
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "qmstop") {
		t.Fatalf("a stop task ID passed as a start: %v", err)
	}
	if _, err := parseTaskID(taskIDFor("vzdump", "9000"), "pve", "destroy"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a backup task ID passed as a destroy: %v", err)
	}
}

func TestATaskIDNamingNoGuestIsRefused(t *testing.T) {
	for _, guest := range []string{"root", "9000x", "-1", "0", "99", "1000000000"} {
		if _, err := parseTaskID(taskIDFor("qmdestroy", guest), "pve", "destroy"); !errors.Is(err, ErrInvalid) {
			t.Fatalf("task ID naming guest %q accepted: %v", guest, err)
		}
	}
}

func TestATaskIDCannotLeaveItsURLPath(t *testing.T) {
	for _, upid := range []string{
		"UPID:pve:00000001:00000000:ABCD:qmstart:9000:api@pve!token:/../../nodes",
		"UPID:pve:00000001:00000000:ABCD:qmstart:9000:api@pve!token:?x=1",
		"UPID:pve:00000001:00000000:ABCD:qmstart:9000:api@pve!token:#fragment",
	} {
		if _, err := parseTaskID(upid, "pve", "start"); !errors.Is(err, ErrInvalid) {
			t.Fatalf("task ID %q accepted into a URL path: %v", upid, err)
		}
	}
}

// Reconciliation is where a caller hands back a task ID it persisted, so the
// refusal has to reach the caller as an invalid input rather than as a state
// the client could not determine.
func TestReconcileRefusesATaskIDFromAnotherOperation(t *testing.T) {
	fake := newFake()
	fake.exists = true
	client, _ := testClient(t, fake)
	_, err := client.Reconcile(context.Background(), testAction, testIdentity, "start", taskIDFor("qmstop", "9000"))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("reconcile replayed a stop task as a start: %v", err)
	}
	_, err = client.Reconcile(context.Background(), testAction, testIdentity, "start", taskIDFor("qmstart", "9000"))
	if !errors.Is(err, ErrUnknownOutcome) && err != nil {
		t.Fatalf("reconcile refused its own start task ID: %v", err)
	}
}
