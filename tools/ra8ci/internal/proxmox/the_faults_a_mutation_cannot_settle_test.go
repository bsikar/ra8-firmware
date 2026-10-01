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

// clientPollingEvery is clientAnswering with the task poll interval opened up,
// so a case can cancel an operation while the client is between task polls
// rather than racing the one-millisecond default.
func clientPollingEvery(t *testing.T, answer http.HandlerFunc, poll time.Duration) *Client {
	t.Helper()
	server := httptest.NewTLSServer(answer)
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
	client, err := New(Config{Endpoint: server.URL, CAFile: ca, TokenFile: token,
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		AllowedVMIDs: []int{9000}, TemplateVMIDs: []int{9001}, Bridges: []string{"vmbr8", "vmbr9"},
		RequestTimeout: 2 * time.Second, OperationTimeout: 30 * time.Second, TaskPollInterval: poll})
	if err != nil {
		t.Fatal(err)
	}
	return client
}

// startingGuest is the shared fake holding one stopped guest, which is the
// state a start acts on.
func startingGuest() *fakePVE {
	f := newFake()
	f.exists = true
	f.status = "stopped"
	return f
}

// answeringStartWith speaks well-formed Proxmox for everything except the
// start itself, where it hands back whatever the case wants as a task ID.
func answeringStartWith(f *fakePVE, upid string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/qemu/9000/status/start") {
			w.Header().Set("Content-Type", "application/json")
			writeData(w, upid)
			return
		}
		f.serve(w, r)
	}
}

// A guest configuration missing a field the client reads never reads as a
// guest with an empty one. Which refusal it draws depends on what the field
// is for: an absent description or name is judged against the reservation and
// comes back a conflict, since a guest that cannot show this reservation's
// marker is exactly how another team's VM looks, while an absent digest has
// nothing to be judged against and is a protocol fault outright.
func TestAGuestConfigurationMissingAFieldIsNeverReadAsEmpty(t *testing.T) {
	for _, one := range []struct {
		absent string
		want   error
	}{
		{"description", ErrConflict},
		{"name", ErrConflict},
		{"digest", ErrProtocol},
	} {
		t.Run("without "+one.absent, func(t *testing.T) {
			client := clientAnswering(t, func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				switch {
				case r.Method == http.MethodGet && r.URL.Path == "/api2/json/cluster/resources":
					writeData(w, []map[string]any{{
						"vmid": 9000, "type": "qemu", "node": "pve", "name": testIdentity.Name,
						"pool": "ra8-tf-lab", "status": "stopped", "template": 0,
					}})
				case r.Method == http.MethodGet && r.URL.Path == "/api2/json/nodes/pve/qemu/9000/config":
					config := map[string]any{
						"name": testIdentity.Name, "description": testIdentity.marker(), "digest": testDigest,
						"protection": 0, "template": 0, "lock": "",
						"scsi0": "ra8-tf-lab:vm-9000-disk-0",
						"net0":  "virtio=AA:BB:CC:DD:EE:01,bridge=vmbr8,firewall=1",
					}
					delete(config, one.absent)
					writeData(w, config)
				default:
					w.WriteHeader(http.StatusNotFound)
				}
			})

			vm, err := client.Get(context.Background(), testIdentity)
			if !errors.Is(err, one.want) {
				t.Fatalf("configuration without %q: err = %v, want %v", one.absent, err, one.want)
			}
			if vm.Status != "" || vm.ConfigDigest != "" {
				t.Errorf("a refused inspection still described a VM: %+v", vm)
			}
		})
	}
}

// A mutation the API accepted but reported with a task ID this client cannot
// hold to the operation leaves the outcome unknown: the request may well have
// taken effect, so the caller is told to reconcile rather than retry.
func TestAMutationReportedWithAnUnusableTaskIDIsUnknown(t *testing.T) {
	for _, one := range []struct {
		name string
		upid string
	}{
		{"no task ID at all", ""},
		{"not a task ID", "started-ok"},
		{"a task ID of another operation", "UPID:pve:00000001:00000000:ABCD:qmstop:9000:api@pve!token:"},
		{"a task ID missing its fields", "UPID:pve:qmstart"},
	} {
		t.Run(one.name, func(t *testing.T) {
			client := clientAnswering(t, answeringStartWith(startingGuest(), one.upid))

			_, err := client.Start(context.Background(), Action{ID: testAction}, testIdentity)
			if !errors.Is(err, ErrUnknownOutcome) {
				t.Fatalf("err = %v, want the outcome reported unknown", err)
			}
			var unknown *UnknownOutcomeError
			if !errors.As(err, &unknown) {
				t.Fatalf("err = %v, want an UnknownOutcomeError a caller can reconcile from", err)
			}
			if unknown.OperationID != testAction {
				t.Errorf("operation ID = %q, want the action's own %q", unknown.OperationID, testAction)
			}
			if unknown.UPID != "" {
				t.Errorf("UPID = %q, want none recorded: the reported one cannot be trusted", unknown.UPID)
			}
			if !strings.Contains(unknown.Cause.Error(), ErrProtocol.Error()) {
				t.Errorf("cause = %v, want the protocol fault named", unknown.Cause)
			}
		})
	}
}

// Giving up on a task that is still running does not undo it. The caller is
// handed the task ID alongside the cancellation, since that is the one thing
// a later reconcile needs.
func TestAbandoningAStillRunningTaskReportsItForReconciliation(t *testing.T) {
	f := startingGuest()
	f.taskRunning = true
	client := clientPollingEvery(t, http.HandlerFunc(f.serve), 400*time.Millisecond)

	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(80 * time.Millisecond)
		cancel()
	}()

	_, err := client.Start(ctx, Action{ID: testAction}, testIdentity)
	if !errors.Is(err, ErrUnknownOutcome) {
		t.Fatalf("err = %v, want the outcome reported unknown", err)
	}
	var unknown *UnknownOutcomeError
	if !errors.As(err, &unknown) {
		t.Fatalf("err = %v, want an UnknownOutcomeError", err)
	}
	if unknown.UPID == "" {
		t.Error("no task ID recorded: a later reconcile has nothing to ask about")
	}
	if !errors.Is(unknown.Cause, context.Canceled) {
		t.Errorf("cause = %v, want the cancellation itself", unknown.Cause)
	}
}
