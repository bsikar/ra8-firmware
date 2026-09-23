// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package scaler turns approved GitHub scale-set events into fenced,
// auditable Proxmox VM lifecycle operations. It never executes job content.
package scaler

import (
	"context"
	"errors"
	"fmt"
	"regexp"
	"strconv"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

var digestPattern = regexp.MustCompile(`^[0-9a-f]{40}$`)
var sha256Pattern = regexp.MustCompile(`^[0-9a-f]{64}$`)
var infrastructurePart = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`)
var runnerName = regexp.MustCompile(`^ra8-lab-[a-z0-9][a-z0-9-]{0,54}$`)

// Options are operator-approved infrastructure facts, never job fields.
// The persistent ra8ci service VM must not be in VMIDs or this pool.
type Options struct {
	Actor             string
	ScaleSetID        int64
	VMIDs             []int
	Node              string
	Pool              string
	Storage           string
	TemplateVMID      int
	TemplateName      string
	TemplateDigest    string
	BackupApprovalID  string
	CleanupApprovalID string
	MaxReconcileBatch int
}

// Metadata is independently fetched from trusted GitHub API state.
type Metadata = github.JobMetadata

type MetadataResolver interface {
	Resolve(context.Context, github.Job) (Metadata, error)
}

// Bootstrapper performs post-boot Ansible readiness, then delivers one-use JIT
// configuration through a protected guest channel. Prepare is called only for
// a running guest and must reconcile idempotently by reservation ID after an
// ambiguous response; it must not put JIT bytes in SQL, logs, or argv.
type Bootstrapper interface {
	Prepare(context.Context, store.RunnerVM) (BootstrapReceipt, error)
}

type BootstrapReceipt struct {
	ReservationID      string
	VMID               int
	CommitSHA          string
	GuestOS            string
	GuestArchitecture  string
	ServiceAccount     string
	RunnerBinarySHA256 string
	AgentBinarySHA256  string
	ReadinessSHA256    string
	JITConfigSHA256    string
	JITConfigExpiresAt time.Time
	EvidenceID         string
	PreparedAt         time.Time
}

// RunnerObserver independently verifies runner registration and deregistration.
// A durable terminal GitHub completion event proves that the bound runner has
// no active job; the observer never treats registration alone as idle proof.
type RunnerObserver interface {
	Registered(context.Context, store.RunnerVM, github.Job) (RunnerObservation, error)
	DrainAndDeregister(context.Context, store.RunnerVM, github.Job) (RunnerObservation, error)
}

type RunnerObservation struct {
	RunnerID           int64
	RunnerName         string
	EvidenceID         string
	ObservedAt         time.Time
	Drained            bool
	NoActiveJob        bool
	RunnerDeregistered bool
}

// BackupGate checks current off-VM database backup/restore readiness before
// new capacity is reserved. Cleanup and reconciliation remain possible when
// this check fails.
type BackupGate interface {
	Check(context.Context, string) error
}

// Provisioner is injected. The production implementation must use approved
// run-scoped Terraform state and Ansible readiness; direct Proxmox control is
// exercised only by test fixtures pending a separate safety review.
// Its current UPID/evidence shape is a migration boundary, not a production
// Terraform interface.
type Provisioner interface {
	Get(context.Context, proxmox.Identity) (proxmox.VM, error)
	Clone(context.Context, proxmox.Action, proxmox.CloneSpec) (proxmox.Result, error)
	Start(context.Context, proxmox.Action, proxmox.Identity) (proxmox.Result, error)
	Stop(context.Context, proxmox.Action, proxmox.Identity, proxmox.IdleProof) (proxmox.Result, error)
	Destroy(context.Context, proxmox.Action, proxmox.Identity, proxmox.DestroyProof) (proxmox.Result, error)
	Reconcile(context.Context, string, proxmox.Identity, string, string) (proxmox.Result, error)
}

// Ledger commits each operation intent before VMClient is invoked. Store
// implements this interface; tests use the same CAS and replay contract.
type Ledger interface {
	GetRunnerVMByJob(context.Context, int64, string) (store.RunnerVM, error)
	GetRunnerVM(context.Context, string) (store.RunnerVM, error)
	GetRunnerVMOperation(context.Context, string) (store.RunnerVMOperation, error)
	ReserveRunnerVM(context.Context, string, store.RunnerVMInput) (store.RunnerVM, bool, error)
	BeginRunnerVMOperation(context.Context, string, string, int64, string, store.RunnerVMSafetyEvidence) (store.RunnerVMOperation, error)
	RecordRunnerVMUPID(context.Context, string, string, int64, string, string) error
	ResolveRunnerVMOperation(context.Context, string, string, int64, string, store.RunnerVMResolution) (store.RunnerVM, error)
	MarkRunnerVMRegistered(context.Context, string, string, int64, int64, string) (store.RunnerVM, error)
	MarkRunnerVMDraining(context.Context, string, string, int64) (store.RunnerVM, error)
	ListUnresolvedRunnerVMs(context.Context, int64, int) ([]store.RunnerVM, error)
	RecordRunnerVMBootstrapEvidence(context.Context, string, store.RunnerVMBootstrapEvidence) error
}

type Handler struct {
	config    Options
	ledger    Ledger
	vms       Provisioner
	metadata  MetadataResolver
	bootstrap Bootstrapper
	runners   RunnerObserver
	backup    BackupGate
	admission github.Admission
}

var _ MetadataResolver = (*github.MetadataResolver)(nil)
var _ github.Handler = (*Handler)(nil)
var _ Ledger = (*store.Store)(nil)

// NewHandler fails closed until every external trust boundary has an
// implementation and the disposable VM/template/backup approvals are exact.
func NewHandler(cfg Options, ledger Ledger, vms Provisioner, metadata MetadataResolver, bootstrap Bootstrapper, runners RunnerObserver, backup BackupGate, admission github.Admission) (*Handler, error) {
	if ledger == nil || vms == nil || metadata == nil || bootstrap == nil || runners == nil || backup == nil || admission == nil ||
		cfg.Actor == "" || len(cfg.Actor) > 256 || cfg.ScaleSetID <= 0 || !infrastructurePart.MatchString(cfg.Node) || !infrastructurePart.MatchString(cfg.Pool) || cfg.Pool == "ra8ci-control" || !infrastructurePart.MatchString(cfg.Storage) ||
		cfg.TemplateVMID < 9000 || !runnerName.MatchString(cfg.TemplateName) || !digestPattern.MatchString(cfg.TemplateDigest) || !store.ValidID(cfg.BackupApprovalID) || !store.ValidID(cfg.CleanupApprovalID) || cfg.BackupApprovalID == cfg.CleanupApprovalID || len(cfg.VMIDs) == 0 {
		return nil, errors.New("scaler requires approved infrastructure, backup, and trusted service interfaces")
	}
	seen := make(map[int]struct{}, len(cfg.VMIDs))
	for _, vmid := range cfg.VMIDs {
		if vmid < 9000 || vmid == cfg.TemplateVMID {
			return nil, errors.New("scaler VMID is outside disposable reservation pool")
		}
		if _, duplicate := seen[vmid]; duplicate {
			return nil, errors.New("scaler VMID allowlist has duplicate")
		}
		seen[vmid] = struct{}{}
	}
	if cfg.MaxReconcileBatch == 0 {
		cfg.MaxReconcileBatch = 64
	}
	if cfg.MaxReconcileBatch < 1 || cfg.MaxReconcileBatch > 1000 {
		return nil, errors.New("scaler reconciliation batch out of range")
	}
	cfg.VMIDs = append([]int(nil), cfg.VMIDs...)
	return &Handler{config: cfg, ledger: ledger, vms: vms, metadata: metadata, bootstrap: bootstrap, runners: runners, backup: backup, admission: admission}, nil
}

func (h *Handler) Process(ctx context.Context, message github.Message) error {
	if h == nil || message.ScaleSetID != int(h.config.ScaleSetID) {
		return errors.New("foreign or unconfigured scale-set message")
	}
	for _, job := range message.Assigned {
		if err := h.admission.Allow(ctx, job); err != nil {
			return err
		}
		if err := h.assigned(ctx, job); err != nil {
			return fmt.Errorf("assigned job %s: %w", job.JobID, err)
		}
	}
	for _, job := range message.Started {
		if err := h.admission.Allow(ctx, job); err != nil {
			return err
		}
		if err := h.started(ctx, job); err != nil {
			return fmt.Errorf("started job %s: %w", job.JobID, err)
		}
	}
	for _, job := range message.Completed {
		if err := h.admission.Allow(ctx, job); err != nil {
			return err
		}
		if err := h.completed(ctx, job); err != nil {
			return fmt.Errorf("completed job %s: %w", job.JobID, err)
		}
	}
	return nil
}

// Reconcile never invents a successful mutation from a desired VM state. It
// only revisits previously committed intents and their Proxmox task/marker.
func (h *Handler) Reconcile(ctx context.Context, _ github.Statistics) error {
	if h == nil {
		return errors.New("unconfigured scaler")
	}
	pending, err := h.ledger.ListUnresolvedRunnerVMs(ctx, h.config.ScaleSetID, h.config.MaxReconcileBatch)
	if err != nil {
		return err
	}
	for _, reservation := range pending {
		if _, err := h.reconcileOne(ctx, reservation); err != nil {
			return fmt.Errorf("reconcile reservation %s: %w", reservation.ID, err)
		}
	}
	return nil
}

func (h *Handler) identity(vm store.RunnerVM) (proxmox.Identity, error) {
	if vm.ScaleSetID != h.config.ScaleSetID || vm.Node != h.config.Node || vm.Pool != h.config.Pool || vm.Storage != h.config.Storage ||
		vm.TemplateVMID != h.config.TemplateVMID || vm.TemplateName != h.config.TemplateName || vm.TemplateDigest != h.config.TemplateDigest || !containsID(h.config.VMIDs, vm.VMID) {
		return proxmox.Identity{}, errors.New("durable VM identity differs from current approvals")
	}
	return proxmox.Identity{VMID: vm.VMID, Node: vm.Node, Pool: vm.Pool, Storage: vm.Storage, Name: vm.Name, ReservationID: vm.ID, CreationOperationID: vm.CreationOperationID}, nil
}

func containsID(ids []int, id int) bool {
	for _, allowed := range ids {
		if allowed == id {
			return true
		}
	}
	return false
}

func (h *Handler) matchesMessage(vm store.RunnerVM, job github.Job) bool {
	return vm.ScaleSetID == h.config.ScaleSetID && vm.JobID == job.JobID &&
		vm.RunnerRequestID == job.RunnerRequestID && vm.WorkflowRunID == job.WorkflowRunID &&
		vm.Repository == job.Owner+"/"+job.Repository && vm.WorkflowRef == job.WorkflowRef
}

func (h *Handler) jobInput(ctx context.Context, job github.Job, vmid int) (store.RunnerVMInput, error) {
	metadata, err := h.metadata.Resolve(ctx, job)
	if err != nil {
		return store.RunnerVMInput{}, err
	}
	if job.JobID == "" || job.RunnerRequestID <= 0 || job.WorkflowRunID <= 0 ||
		metadata.JobID != job.JobID || metadata.WorkflowRunID != job.WorkflowRunID || metadata.Repository != job.Owner+"/"+job.Repository || metadata.WorkflowAttempt <= 0 || !digestPattern.MatchString(metadata.CommitSHA) {
		return store.RunnerVMInput{}, errors.New("trusted GitHub metadata does not match scale-set job")
	}
	return store.RunnerVMInput{ScaleSetID: h.config.ScaleSetID, JobID: job.JobID, RunnerRequestID: job.RunnerRequestID,
		WorkflowRunID: job.WorkflowRunID, WorkflowAttempt: metadata.WorkflowAttempt,
		Repository: metadata.Repository, WorkflowRef: job.WorkflowRef, CommitSHA: metadata.CommitSHA,
		VMID: vmid, Node: h.config.Node, Pool: h.config.Pool, Storage: h.config.Storage,
		Name: "ra8-lab-ci-" + strconv.Itoa(vmid), TemplateVMID: h.config.TemplateVMID,
		TemplateName: h.config.TemplateName, TemplateDigest: h.config.TemplateDigest}, nil
}

func (h *Handler) reservation(ctx context.Context, job github.Job) (store.RunnerVM, error) {
	existing, err := h.ledger.GetRunnerVMByJob(ctx, h.config.ScaleSetID, job.JobID)
	if err == nil {
		input, err := h.jobInput(ctx, job, existing.VMID)
		if err != nil || input != existing.RunnerVMInput {
			return store.RunnerVM{}, errors.New("replayed GitHub job changed immutable VM reservation")
		}
		_, err = h.identity(existing)
		return existing, err
	}
	if !errors.Is(err, store.ErrNotFound) {
		return store.RunnerVM{}, err
	}
	if err := h.backup.Check(ctx, h.config.BackupApprovalID); err != nil {
		return store.RunnerVM{}, fmt.Errorf("off-VM backup gate: %w", err)
	}
	for _, vmid := range h.config.VMIDs {
		input, err := h.jobInput(ctx, job, vmid)
		if err != nil {
			return store.RunnerVM{}, err
		}
		vm, _, err := h.ledger.ReserveRunnerVM(ctx, h.config.Actor, input)
		if err == nil {
			return vm, nil
		}
		if !errors.Is(err, store.ErrConflict) {
			return store.RunnerVM{}, err
		}
		concurrent, lookupErr := h.ledger.GetRunnerVMByJob(ctx, h.config.ScaleSetID, job.JobID)
		if lookupErr == nil {
			if concurrent.RunnerVMInput != input {
				return store.RunnerVM{}, errors.New("concurrent reservation changed job identity")
			}
			return concurrent, nil
		}
		if !errors.Is(lookupErr, store.ErrNotFound) {
			return store.RunnerVM{}, lookupErr
		}
	}
	return store.RunnerVM{}, errors.New("no approved disposable VMID is free")
}
