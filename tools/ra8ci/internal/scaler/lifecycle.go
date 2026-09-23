// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func (h *Handler) assigned(ctx context.Context, job github.Job) error {
	vm, err := h.reservation(ctx, job)
	if err != nil {
		return err
	}
	if vm.CleanupRequested || vm.State == "released" {
		return nil // a late/replayed assignment never undoes completion
	}
	if vm.UnknownOutcome {
		vm, err = h.reconcileOne(ctx, vm)
		if err != nil {
			return err
		}
	}
	if vm.CleanupRequested {
		return nil
	}
	if vm.State == "reserved" || vm.State == "stopped" {
		if err := h.backup.Check(ctx, h.config.BackupApprovalID); err != nil {
			return fmt.Errorf("off-VM backup gate before VM launch: %w", err)
		}
	}
	if vm.State == "reserved" {
		vm, err = h.execute(ctx, vm, "clone", store.RunnerVMSafetyEvidence{})
		if err != nil {
			return err
		}
	}
	if vm.State == "stopped" && !vm.CleanupRequested {
		return h.prepareAndStart(ctx, vm)
	}
	if vm.State == "running" && !vm.CleanupRequested {
		return h.bootstrapRunning(ctx, vm)
	}
	if vm.State == "registered" || vm.State == "draining" {
		return nil
	}
	return fmt.Errorf("assigned job has unexpected VM state %q", vm.State)
}

func (h *Handler) prepareAndStart(ctx context.Context, vm store.RunnerVM) error {
	identity, err := h.identity(vm)
	if err != nil {
		return err
	}
	observed, err := h.vms.Get(ctx, identity)
	if err != nil || observed.Status != "stopped" || observed.Locked {
		if err != nil {
			return err
		}
		return errors.New("runner guest is not stable and stopped before start")
	}
	started, err := h.execute(ctx, vm, "start", store.RunnerVMSafetyEvidence{})
	if err != nil {
		return err
	}
	return h.bootstrapRunning(ctx, started)
}

func (h *Handler) bootstrapRunning(ctx context.Context, vm store.RunnerVM) error {
	if vm.State != "running" || vm.UnknownOutcome || vm.CleanupRequested {
		return errors.New("JIT bootstrap requires a reconciled running reservation")
	}
	receipt, err := h.bootstrap.Prepare(ctx, vm)
	if err != nil {
		return fmt.Errorf("post-boot Ansible readiness and JIT bootstrap: %w", err)
	}
	now := time.Now()
	if receipt.ReservationID != vm.ID || receipt.VMID != vm.VMID || receipt.CommitSHA != vm.CommitSHA ||
		!store.ValidID(receipt.EvidenceID) || !sha256Pattern.MatchString(receipt.ReadinessSHA256) ||
		!sha256Pattern.MatchString(receipt.RunnerBinarySHA256) || !sha256Pattern.MatchString(receipt.AgentBinarySHA256) ||
		!sha256Pattern.MatchString(receipt.JITConfigSHA256) ||
		(receipt.GuestOS != "linux" && receipt.GuestOS != "windows") ||
		(receipt.GuestArchitecture != "amd64" && receipt.GuestArchitecture != "arm64") ||
		receipt.ServiceAccount != "ra8ci" || receipt.PreparedAt.IsZero() ||
		receipt.PreparedAt.After(now.Add(time.Second)) || now.Sub(receipt.PreparedAt) > 5*time.Minute ||
		!receipt.JITConfigExpiresAt.After(now) {
		return errors.New("JIT bootstrap receipt lacks fresh identity-bound readiness evidence")
	}
	return h.ledger.RecordRunnerVMBootstrapEvidence(ctx, h.config.Actor, store.RunnerVMBootstrapEvidence{
		ReservationID: receipt.ReservationID, VMID: receipt.VMID, CommitSHA: receipt.CommitSHA,
		GuestOS: receipt.GuestOS, GuestArchitecture: receipt.GuestArchitecture, ServiceAccount: receipt.ServiceAccount,
		RunnerBinarySHA256: receipt.RunnerBinarySHA256, AgentBinarySHA256: receipt.AgentBinarySHA256,
		ReadinessSHA256: receipt.ReadinessSHA256, JITConfigSHA256: receipt.JITConfigSHA256,
		JITConfigExpiresAt: receipt.JITConfigExpiresAt, EvidenceID: receipt.EvidenceID, PreparedAt: receipt.PreparedAt,
	})
}

func (h *Handler) started(ctx context.Context, job github.Job) error {
	vm, err := h.ledger.GetRunnerVMByJob(ctx, h.config.ScaleSetID, job.JobID)
	if err != nil {
		return err
	}
	if _, err := h.identity(vm); err != nil {
		return err
	}
	if !h.matchesMessage(vm, job) {
		return errors.New("started event does not match durable GitHub job")
	}
	if vm.CleanupRequested {
		return nil
	}
	if vm.UnknownOutcome {
		vm, err = h.reconcileOne(ctx, vm)
		if err != nil {
			return err
		}
	}
	if vm.State == "registered" {
		if int64(job.RunnerID) != vm.ExternalRunnerID || job.RunnerName != vm.ExternalRunnerName {
			return errors.New("started message disagrees with durable runner identity")
		}
		return nil
	}
	if vm.State != "running" || job.RunnerID <= 0 || job.RunnerName == "" {
		return errors.New("started job lacks a running owned runner")
	}
	evidence, err := h.runners.Registered(ctx, vm, job)
	if err != nil {
		return err
	}
	if evidence.RunnerID != int64(job.RunnerID) || evidence.RunnerName != job.RunnerName ||
		!store.ValidID(evidence.EvidenceID) || !fresh(evidence.ObservedAt, 10*time.Second) {
		return errors.New("independent runner registration does not match the job")
	}
	_, err = h.ledger.MarkRunnerVMRegistered(ctx, h.config.Actor, vm.ID, vm.Generation, evidence.RunnerID, evidence.RunnerName)
	return err
}

func (h *Handler) completed(ctx context.Context, job github.Job) error {
	vm, err := h.ledger.GetRunnerVMByJob(ctx, h.config.ScaleSetID, job.JobID)
	if errors.Is(err, store.ErrNotFound) {
		return nil // no VM was ever reserved for this job
	}
	if err != nil {
		return err
	}
	if _, err := h.identity(vm); err != nil {
		return err
	}
	if !h.matchesMessage(vm, job) {
		return errors.New("completed event does not match durable GitHub job")
	}
	if vm.State == "released" {
		return nil
	}
	if !vm.CleanupRequested {
		vm, err = h.ledger.MarkRunnerVMDraining(ctx, h.config.Actor, vm.ID, vm.Generation)
		if err != nil {
			return err
		}
	}
	if vm.UnknownOutcome {
		vm, err = h.reconcileOne(ctx, vm)
		if err != nil {
			return err
		}
	}
	if vm.State == "reserved" {
		return errors.New("never-created runner reservation requires operator absence attestation before abandonment")
	}
	if vm.State == "draining" {
		proof, err := h.drainEvidence(ctx, vm, false)
		if err != nil {
			return err
		}
		vm, err = h.execute(ctx, vm, "stop", proof)
		if err != nil {
			return err
		}
	}
	if vm.State != "stopped" || !vm.CleanupRequested {
		return fmt.Errorf("completed job has unexpected VM state %q", vm.State)
	}
	proof, err := h.drainEvidence(ctx, vm, true)
	if err != nil {
		return err
	}
	_, err = h.execute(ctx, vm, "destroy", proof)
	return err
}

func fresh(observed time.Time, maxAge time.Duration) bool {
	now := time.Now()
	return !observed.IsZero() && !observed.After(now.Add(time.Second)) && now.Sub(observed) <= maxAge
}

func (h *Handler) drainEvidence(ctx context.Context, vm store.RunnerVM, destroy bool) (store.RunnerVMSafetyEvidence, error) {
	evidence, err := h.runners.DrainAndDeregister(ctx, vm)
	if err != nil {
		return store.RunnerVMSafetyEvidence{}, err
	}
	if !store.ValidID(evidence.EvidenceID) || !fresh(evidence.ObservedAt, 10*time.Second) || !evidence.Drained || !evidence.NoActiveJob ||
		(vm.ExternalRunnerID != 0 && evidence.RunnerID != vm.ExternalRunnerID) ||
		(vm.ExternalRunnerName != "" && evidence.RunnerName != vm.ExternalRunnerName) {
		return store.RunnerVMSafetyEvidence{}, errors.New("runner drain lacks fresh exact ownership and idle evidence")
	}
	proof := store.RunnerVMSafetyEvidence{EvidenceID: evidence.EvidenceID, ObservedAt: evidence.ObservedAt,
		Drained: true, NoActiveJob: true, RunnerDeregistered: evidence.RunnerDeregistered, ExternalRunnerID: evidence.RunnerID}
	if destroy {
		if !evidence.RunnerDeregistered || !store.ValidID(evidence.ApprovalID) {
			return store.RunnerVMSafetyEvidence{}, errors.New("reviewed deregistration and cleanup approval required")
		}
		identity, err := h.identity(vm)
		if err != nil {
			return store.RunnerVMSafetyEvidence{}, err
		}
		observed, err := h.vms.Get(ctx, identity)
		if err != nil {
			return store.RunnerVMSafetyEvidence{}, err
		}
		if observed.Status != "stopped" || observed.Protected || observed.Locked || !digestPattern.MatchString(observed.ConfigDigest) {
			return store.RunnerVMSafetyEvidence{}, errors.New("guest is not safe for reviewed cleanup")
		}
		proof.ExpectedConfigDigest = observed.ConfigDigest
		proof.ApprovalID = evidence.ApprovalID
	}
	return proof, nil
}

func (h *Handler) execute(ctx context.Context, vm store.RunnerVM, kind string, proof store.RunnerVMSafetyEvidence) (store.RunnerVM, error) {
	identity, err := h.identity(vm)
	if err != nil {
		return store.RunnerVM{}, err
	}
	op, err := h.ledger.BeginRunnerVMOperation(ctx, h.config.Actor, vm.ID, vm.Generation, kind, proof)
	if err != nil {
		return store.RunnerVM{}, err
	}
	if op.PriorRequestIssued {
		return h.reconcileOperation(ctx, vm, op)
	}
	action := proxmox.Action{ID: op.ID}
	var result proxmox.Result
	switch kind {
	case "clone":
		result, err = h.vms.Clone(ctx, action, proxmox.CloneSpec{Target: identity, TemplateVMID: vm.TemplateVMID, TemplateName: vm.TemplateName, TemplateDigest: vm.TemplateDigest})
	case "start":
		result, err = h.vms.Start(ctx, action, identity)
	case "stop":
		result, err = h.vms.Stop(ctx, action, identity, proxmox.IdleProof{VMID: vm.VMID, ReservationID: vm.ID,
			EvidenceID: proof.EvidenceID, ObservedAt: proof.ObservedAt, Drained: proof.Drained, NoActiveJob: proof.NoActiveJob})
	case "destroy":
		result, err = h.vms.Destroy(ctx, action, identity, proxmox.DestroyProof{IdleProof: proxmox.IdleProof{VMID: vm.VMID, ReservationID: vm.ID,
			EvidenceID: proof.EvidenceID, ObservedAt: proof.ObservedAt, Drained: proof.Drained, NoActiveJob: proof.NoActiveJob},
			ApprovalID: proof.ApprovalID, ExpectedConfigDigest: proof.ExpectedConfigDigest,
			RunnerDeregistered: proof.RunnerDeregistered, StateReconciled: true})
	default:
		return store.RunnerVM{}, errors.New("unsupported VM operation")
	}
	var unknown *proxmox.UnknownOutcomeError
	if errors.As(err, &unknown) && unknown.UPID != "" {
		if recordErr := h.ledger.RecordRunnerVMUPID(ctx, h.config.Actor, vm.ID, op.Generation, op.ID, unknown.UPID); recordErr != nil {
			return store.RunnerVM{}, fmt.Errorf("mutation unknown and UPID not durable: %w", recordErr)
		}
	}
	if err != nil {
		return store.RunnerVM{}, err // committed intent stays unknown; never auto-retry
	}
	return h.resolveVerified(ctx, vm, op, result)
}

func (h *Handler) reconcileOne(ctx context.Context, vm store.RunnerVM) (store.RunnerVM, error) {
	if !vm.UnknownOutcome || vm.CurrentOperationID == "" {
		return vm, nil
	}
	if _, err := h.identity(vm); err != nil {
		return store.RunnerVM{}, err
	}
	op, err := h.ledger.GetRunnerVMOperation(ctx, vm.CurrentOperationID)
	if err != nil {
		return store.RunnerVM{}, err
	}
	if op.RunnerVMID != vm.ID || op.Generation != vm.Generation || op.Status != "unresolved" {
		return store.RunnerVM{}, errors.New("operation ledger is not fenced to reservation")
	}
	return h.reconcileOperation(ctx, vm, op)
}

func (h *Handler) reconcileOperation(ctx context.Context, vm store.RunnerVM, op store.RunnerVMOperation) (store.RunnerVM, error) {
	identity, err := h.identity(vm)
	if err != nil {
		return store.RunnerVM{}, err
	}
	result, err := h.vms.Reconcile(ctx, op.ID, identity, op.Kind, op.UPID)
	if err != nil {
		return store.RunnerVM{}, err
	}
	return h.resolveVerified(ctx, vm, op, result)
}

func (h *Handler) resolveVerified(ctx context.Context, vm store.RunnerVM, op store.RunnerVMOperation, result proxmox.Result) (store.RunnerVM, error) {
	if proof := result.TerraformPreflightNoEffect; proof != nil {
		now := time.Now()
		if result.TerraformEvidence != nil || result.UPID != "" ||
			(op.ProviderKind != "proxmox" && op.ProviderKind != "terraform") ||
			op.UPID != "" || op.TerraformApplyStartedAt != nil ||
			proof.ObservedAt.IsZero() || proof.ObservedAt.After(now.Add(time.Second)) ||
			now.Sub(proof.ObservedAt) > 30*time.Second {
			return store.RunnerVM{}, errors.New("invalid or stale Terraform preflight no-effect evidence")
		}
		if (op.ProviderKind == "proxmox" && (op.PlanSHA256 != "" || proof.PlanSHA256 != "" || proof.StateIdentitySHA256 != "")) ||
			(op.ProviderKind == "terraform" && (op.PlanSHA256 == "" || proof.PlanSHA256 != op.PlanSHA256 || proof.StateIdentitySHA256 != op.StateIdentitySHA256)) {
			return store.RunnerVM{}, errors.New("Terraform preflight evidence does not match the durable plan")
		}
		evidenceID, err := store.NewID()
		if err != nil {
			return store.RunnerVM{}, err
		}
		resolved, err := h.ledger.ResolveRunnerVMOperation(ctx, h.config.Actor, vm.ID, op.Generation, op.ID,
			store.RunnerVMResolution{Outcome: "failed", EvidenceID: evidenceID,
				Source: "terraform_preflight", ObservedAt: proof.ObservedAt, PostStateVerified: true,
				PlanSHA256: proof.PlanSHA256, StateIdentitySHA256: proof.StateIdentitySHA256})
		if err != nil {
			return store.RunnerVM{}, err
		}
		return resolved, errors.New("Terraform preflight failed before any apply was authorized")
	}
	if evidence := result.TerraformEvidence; evidence != nil {
		now := time.Now()
		if result.UPID != "" || (evidence.Outcome != "succeeded" && evidence.Outcome != "failed") ||
			op.ProviderKind != "terraform" || evidence.PlanSHA256 != op.PlanSHA256 ||
			evidence.StateIdentitySHA256 != op.StateIdentitySHA256 ||
			!sha256Pattern.MatchString(evidence.PlanSHA256) ||
			!sha256Pattern.MatchString(evidence.StateIdentitySHA256) ||
			!sha256Pattern.MatchString(evidence.ReconciliationSHA256) ||
			evidence.ObservedAt.IsZero() || evidence.ObservedAt.After(now.Add(time.Second)) ||
			now.Sub(evidence.ObservedAt) > 30*time.Second {
			return store.RunnerVM{}, errors.New("invalid or stale Terraform state evidence")
		}
		evidenceID, err := store.NewID()
		if err != nil {
			return store.RunnerVM{}, err
		}
		resolved, err := h.ledger.ResolveRunnerVMOperation(ctx, h.config.Actor, vm.ID, op.Generation, op.ID,
			store.RunnerVMResolution{Outcome: evidence.Outcome, EvidenceID: evidenceID,
				Source: "terraform_state", ObservedAt: evidence.ObservedAt, PostStateVerified: true,
				PlanSHA256: evidence.PlanSHA256, StateIdentitySHA256: evidence.StateIdentitySHA256,
				ReconciliationSHA256: evidence.ReconciliationSHA256,
				TerraformStateHasVM:  evidence.StateHasVM, TerraformVMAbsent: evidence.VMAbsent,
				TerraformVMStatus: evidence.VMStatus})
		if err != nil {
			return store.RunnerVM{}, err
		}
		if evidence.Outcome == "failed" {
			return resolved, errors.New("Terraform operation was reconciled as having no effect")
		}
		return resolved, nil
	}
	source := "upid"
	if result.UPID != "" {
		if err := h.ledger.RecordRunnerVMUPID(ctx, h.config.Actor, vm.ID, op.Generation, op.ID, result.UPID); err != nil {
			return store.RunnerVM{}, fmt.Errorf("verified Proxmox UPID not durable: %w", err)
		}
	} else if op.Kind == "clone" && result.AlreadySatisfied && result.VM != nil {
		source = "clone_marker"
	} else {
		return store.RunnerVM{}, errors.New("VM state matched without a verifiable Proxmox task; operator reconciliation required")
	}
	evidenceID, err := store.NewID()
	if err != nil {
		return store.RunnerVM{}, err
	}
	return h.ledger.ResolveRunnerVMOperation(ctx, h.config.Actor, vm.ID, op.Generation, op.ID,
		store.RunnerVMResolution{Outcome: "succeeded", EvidenceID: evidenceID, Source: source,
			ObservedAt: time.Now(), PostStateVerified: true})
}
