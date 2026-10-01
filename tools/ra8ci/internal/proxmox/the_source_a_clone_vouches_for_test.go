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

// A clone is the one request in this client that brings a guest into
// existence, so everything it checks first is a guard against cloning from
// something nobody reviewed, or cloning a second time over a guest that is
// already there. Each refusal below has to happen BEFORE the clone request is
// sent; a refusal after it would leave a guest nobody is tracking.

// scriptedLab answers the two reads a clone makes before it sends anything,
// and counts what it was asked to do, so a refusal can be checked to have
// sent nothing.
type scriptedLab struct {
	mu             sync.Mutex
	resources      string
	resourceStatus int
	configStatus   int
	config         string
	reads          int
	mutations      int
}

func (l *scriptedLab) serve(w http.ResponseWriter, r *http.Request) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if r.Method != http.MethodGet {
		l.mutations++
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"data":"` + fakeUPID("qmclone") + `"}`))
		return
	}
	switch r.URL.Path {
	case "/api2/json/cluster/resources":
		l.reads++
		if l.resourceStatus != 0 {
			w.WriteHeader(l.resourceStatus)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(l.resources))
	case "/api2/json/nodes/pve/qemu/9001/config":
		l.reads++
		if l.configStatus != 0 {
			w.WriteHeader(l.configStatus)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(l.config))
	default:
		w.WriteHeader(http.StatusNotFound)
	}
}

func (l *scriptedLab) counts() (int, int) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.reads, l.mutations
}

const approvedTemplate = `{"data":[{"vmid":9001,"type":"qemu","node":"pve","name":"ra8-lab-template","pool":"ra8-tf-lab","status":"stopped","template":1}]}`

func approvedSpec() CloneSpec {
	return CloneSpec{Target: testIdentity, TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest}
}

// The arguments a clone refuses without asking the hypervisor anything at all.
// These are the operator's own mistakes, and none of them is worth a request.
func TestACloneRefusesItsOwnArgumentsBeforeAsking(t *testing.T) {
	for _, attempt := range []struct {
		name string
		edit func(*Action, *CloneSpec)
		says string
	}{
		{"a target outside the approvals", func(_ *Action, s *CloneSpec) { s.Target.VMID = 8999 }, "invalid"},
		{"an operation ID that is not a UUID", func(a *Action, _ *CloneSpec) { a.ID = "clone-1" }, "canonical UUID"},
		{"an operation that is not the reservation's creation marker", func(a *Action, _ *CloneSpec) { a.ID = testAction }, "creation marker"},
		{"a source template nobody approved", func(_ *Action, s *CloneSpec) { s.TemplateVMID = 9002 }, "not approved"},
		{"a source template that is the target itself", func(_ *Action, s *CloneSpec) { s.TemplateVMID = 9000 }, "not approved"},
		{"a source name outside the lab's own", func(_ *Action, s *CloneSpec) { s.TemplateName = "ubuntu-cloud" }, "not approved"},
		{"a digest that is not one", func(_ *Action, s *CloneSpec) { s.TemplateDigest = "latest" }, "not approved"},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			lab := &scriptedLab{resources: approvedTemplate}
			client := clientAnswering(t, lab.serve)
			action, spec := Action{ID: testCreation}, approvedSpec()
			attempt.edit(&action, &spec)

			result, err := client.Clone(context.Background(), action, spec)
			if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), attempt.says) {
				t.Fatalf("error = %v, want an invalid-input refusal naming %q", err, attempt.says)
			}
			if result.VM != nil || result.AlreadySatisfied {
				t.Fatalf("a refused clone still answered: %+v", result)
			}
			if reads, mutations := lab.counts(); reads != 0 || mutations != 0 {
				t.Fatalf("a refused clone still spoke to the hypervisor: %d reads, %d mutations", reads, mutations)
			}
		})
	}
}

// What the lab has to say about the source template, and what each answer
// costs. None of these sends a clone.
func TestACloneRefusesASourceItCannotVouchFor(t *testing.T) {
	for _, attempt := range []struct {
		name string
		lab  *scriptedLab
		want error
		says string
	}{
		{"the listing cannot be read at all",
			&scriptedLab{resourceStatus: http.StatusInternalServerError}, ErrUnavailable, "500"},
		{"the template is not listed",
			&scriptedLab{resources: `{"data":[{"vmid":9199,"type":"qemu","node":"pve","name":"someone-else","pool":"other","status":"running","template":0}]}`},
			ErrConflict, "identity mismatch"},
		{"the template is running",
			&scriptedLab{resources: strings.Replace(approvedTemplate, `"stopped"`, `"running"`, 1)},
			ErrConflict, "identity mismatch"},
		{"the template is not a template",
			&scriptedLab{resources: strings.Replace(approvedTemplate, `"template":1`, `"template":0`, 1)},
			ErrConflict, "identity mismatch"},
		{"the template flag cannot be read",
			&scriptedLab{resources: strings.Replace(approvedTemplate, `"template":1`, `"template":"yes"`, 1)},
			ErrConflict, "identity mismatch"},
		{"the template sits on another node",
			&scriptedLab{resources: strings.Replace(approvedTemplate, `"node":"pve"`, `"node":"pve2"`, 1)},
			ErrConflict, "identity mismatch"},
		{"the template's own config cannot be read",
			&scriptedLab{resources: approvedTemplate, configStatus: http.StatusInternalServerError},
			ErrUnavailable, "500"},
	} {
		t.Run(attempt.name, func(t *testing.T) {
			client := clientAnswering(t, attempt.lab.serve)

			result, err := client.Clone(context.Background(), Action{ID: testCreation}, approvedSpec())
			if !errors.Is(err, attempt.want) {
				t.Fatalf("error = %v, want %v", err, attempt.want)
			}
			if !strings.Contains(err.Error(), attempt.says) {
				t.Fatalf("the refusal read %v, wanted it to name %q", err, attempt.says)
			}
			if result.VM != nil || result.AlreadySatisfied {
				t.Fatalf("a refused clone still answered: %+v", result)
			}
			if _, mutations := attempt.lab.counts(); mutations != 0 {
				t.Fatalf("a clone was sent against a source the lab could not vouch for: %d", mutations)
			}
		})
	}
}

// A guest already at the target ID is never cloned over. If it is ours and
// settled, the clone is an observation; if it is locked, the outcome is
// unknown and a reconcile is owed; if it is someone else's, it is a conflict.
func TestACloneNeverWritesOverAGuestAlreadyThere(t *testing.T) {
	t.Run("ours and settled", func(t *testing.T) {
		f := newFake()
		f.exists = true
		client, _ := testClient(t, f)

		result, err := client.Clone(context.Background(), Action{ID: testCreation}, approvedSpec())
		if err != nil {
			t.Fatalf("an existing exact guest was refused: %v", err)
		}
		if !result.AlreadySatisfied || result.VM == nil || result.VM.Identity.VMID != 9000 {
			t.Fatalf("an existing exact guest was not reported as satisfied: %+v", result)
		}
	})

	t.Run("ours but locked", func(t *testing.T) {
		f := newFake()
		f.exists = true
		f.lock = "clone"
		client, _ := testClient(t, f)

		_, err := client.Clone(context.Background(), Action{ID: testCreation}, approvedSpec())
		var unknown *UnknownOutcomeError
		if !errors.As(err, &unknown) {
			t.Fatalf("error = %v, want an unknown outcome", err)
		}
		if unknown.OperationID != testCreation {
			t.Fatalf("the unknown outcome named operation %q", unknown.OperationID)
		}
	})

	t.Run("someone else's", func(t *testing.T) {
		f := newFake()
		f.exists = true
		f.pool = "other"
		client, _ := testClient(t, f)

		if _, err := client.Clone(context.Background(), Action{ID: testCreation}, approvedSpec()); !errors.Is(err, ErrConflict) {
			t.Fatalf("error = %v, want a conflict over a foreign guest", err)
		}
	})
}

// A prior request on the same operation is the one refusal that is NOT a
// mistake: the outcome of that earlier clone is unknown, so this one is owed a
// reconcile rather than a second request.
func TestACloneReplayedOverAPriorRequestAsksForAReconcile(t *testing.T) {
	lab := &scriptedLab{resources: approvedTemplate}
	client := clientAnswering(t, lab.serve)

	_, err := client.Clone(context.Background(), Action{ID: testCreation, PriorRequestIssued: true}, approvedSpec())
	var unknown *UnknownOutcomeError
	if !errors.As(err, &unknown) {
		t.Fatalf("error = %v, want an unknown outcome", err)
	}
	if reads, mutations := lab.counts(); reads != 0 || mutations != 0 {
		t.Fatalf("a replay spoke to the hypervisor: %d reads, %d mutations", reads, mutations)
	}
}
