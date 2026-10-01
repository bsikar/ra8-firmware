//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheWordAnOperatorMayGive pins the two human-or-marker
// resolution sources (runner_vms.go:742-749) end to end against the real
// schema: a clone marker settles only a Proxmox clone, and an operator
// approval settles any unresolved operation, Proxmox or Terraform, as long
// as it names a valid approval and claims no reconciliation digest. Each resolution records its source and
// approval and returns the VM to the state its outcome implies.
func TestIntegrationTheWordAnOperatorMayGive(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	planSHA := strings.Repeat("a", 64)
	awaiting := func(t *testing.T, terraform bool) (RunnerVM, RunnerVMOperation) {
		t.Helper()
		vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatal(err)
		}
		if terraform {
			if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, RunnerVMTerraformPlanEvidence{
				TerraformVersion: "1.10.5", PlanSHA256: planSHA, ModuleSHA256: strings.Repeat("b", 64),
				InputSHA256: strings.Repeat("c", 64), ProviderLockSHA256: strings.Repeat("d", 64),
				StateIdentitySHA256: strings.Repeat("e", 64), PreparedAt: time.Now().UTC()}); err != nil {
				t.Fatal(err)
			}
		}
		return vm, op
	}
	word := func(t *testing.T, source, outcome string) RunnerVMResolution {
		t.Helper()
		proof := vmResolution(t)
		proof.Source, proof.Outcome, proof.ObservedAt = source, outcome, time.Now().UTC()
		if source == "operator" {
			proof.OperatorApprovalID = mustID(t)
		}
		return proof
	}
	settled := func(t *testing.T, opID string) (status, source, approval string) {
		t.Helper()
		var src, appr *string
		if err := pool.QueryRow(ctx, `SELECT status,resolution_source,resolution_approval_id::text
			FROM runner_vm_operations WHERE id=$1`, opID).Scan(&status, &src, &appr); err != nil {
			t.Fatal(err)
		}
		if src != nil {
			source = *src
		}
		if appr != nil {
			approval = *appr
		}
		return status, source, approval
	}

	t.Run("a word that names no approval or the wrong operation is refused", func(t *testing.T) {
		vm, op := awaiting(t, false)
		for name, proof := range map[string]RunnerVMResolution{
			"an operator with no approval": func() RunnerVMResolution { p := word(t, "operator", "failed"); p.OperatorApprovalID = ""; return p }(),
			"an operator with a bad id":    func() RunnerVMResolution { p := word(t, "operator", "failed"); p.OperatorApprovalID = "nope"; return p }(),
			"an operator claiming a reconciliation": func() RunnerVMResolution {
				p := word(t, "operator", "failed")
				p.ReconciliationSHA256 = strings.Repeat("f", 64)
				return p
			}(),
		} {
			if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof); !errors.Is(err, ErrDenied) {
				t.Fatalf("%s: want ErrDenied, got %v", name, err)
			}
		}
		tvm, top := awaiting(t, true)
		digested := word(t, "operator", "succeeded")
		digested.ReconciliationSHA256 = strings.Repeat("f", 64)
		if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", tvm.ID, top.Generation, top.ID, digested); !errors.Is(err, ErrDenied) {
			t.Fatalf("an operator claiming a reconciliation over a Terraform clone: %v", err)
		}
		if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", tvm.ID, top.Generation, top.ID, word(t, "clone_marker", "succeeded")); !errors.Is(err, ErrDenied) {
			t.Fatalf("a clone marker over a Terraform clone: %v", err)
		}
		for _, id := range []string{op.ID, top.ID} {
			if status, _, _ := settled(t, id); status != "unresolved" {
				t.Fatalf("a refused word settled %s: %s", id, status)
			}
		}
	})

	t.Run("a clone marker settles a Proxmox clone", func(t *testing.T) {
		vm, op := awaiting(t, false)
		got, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, word(t, "clone_marker", "succeeded"))
		if err != nil {
			t.Fatal(err)
		}
		if got.State != "stopped" || got.Generation != op.Generation+1 || got.UnknownOutcome {
			t.Fatalf("clone marker left %+v", got)
		}
		if status, source, approval := settled(t, op.ID); status != "succeeded" || source != "clone_marker" || approval != "" {
			t.Fatalf("clone marker recorded %s/%s/%q", status, source, approval)
		}
	})

	for _, c := range []struct {
		name      string
		terraform bool
		outcome   string
		want      string
	}{
		{"an operator settles a Proxmox clone as failed", false, "failed", "reserved"},
		{"an operator settles a Proxmox clone as succeeded", false, "succeeded", "stopped"},
		{"an operator settles a Terraform clone as failed", true, "failed", "reserved"},
		{"an operator settles a Terraform clone as succeeded", true, "succeeded", "stopped"},
	} {
		t.Run(c.name, func(t *testing.T) {
			vm, op := awaiting(t, c.terraform)
			proof := word(t, "operator", c.outcome)
			got, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof)
			if err != nil {
				t.Fatal(err)
			}
			if got.State != c.want || got.Generation != op.Generation+1 || got.UnknownOutcome || got.CurrentOperationID != "" {
				t.Fatalf("operator left %+v, want %s", got, c.want)
			}
			if status, source, approval := settled(t, op.ID); status != c.outcome || source != "operator" || approval != proof.OperatorApprovalID {
				t.Fatalf("operator recorded %s/%s/%q", status, source, approval)
			}
		})
	}
}
