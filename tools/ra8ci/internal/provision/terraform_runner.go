// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

var terraformRunnerVersionPattern = regexp.MustCompile("^[0-9]+[.][0-9]+[.][0-9]+$")
var terraformRunnerLineagePattern = regexp.MustCompile("^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")
var terraformRunnerConfigDigestPattern = regexp.MustCompile("^[0-9a-f]{40}$")
var terraformRunnerNodePattern = regexp.MustCompile("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")

// reviewedRunnerBridges is the closed set of guest bridges a Terraform runner
// profile may name. It stays a literal here, and it is deliberately NOT read
// from operator configuration the way proxmox.Config.Bridges is.
//
// The two checks look alike and answer different questions. This one is an
// admission rule over a reviewed module: a profile is written by review, and
// the literal pair IS the review, so an operator who could widen it could
// place a disposable runner on a network review never saw. The client's
// allowlist is a runtime observation check that must serve any lab an operator
// stands up, so it cannot hardcode this pair without owning a policy that
// belongs to this module.
//
// Both ends still refuse vmbr0 by name, and this set is a subset of any
// allowlist the client would accept. Widening it is a review decision, not a
// deployment one.
var reviewedRunnerBridges = []string{"vmbr8", "vmbr9"}

func reviewedRunnerBridge(name string) bool {
	return slices.Contains(reviewedRunnerBridges, name)
}

// runnerBridgeSegment is the third octet of the lab network a reviewed bridge
// carries: vmbr8 carries 10.250.8.0/24 and vmbr9 carries 10.250.9.0/24, the
// pairing every lab script already assumes (scripts/dev/proxmox_lab_ci.sh,
// proxmox_lab_server_runner.sh, prepare_proxmox_lab_windows_template.sh).
//
// It is read out of the bridge name rather than kept in a second table beside
// reviewedRunnerBridges, because a table is a place for the two to disagree: a
// bridge added to the reviewed set without its row would acquire segment zero
// and refuse every address, and a row edited without its bridge would bind a
// segment nothing carries.
func runnerBridgeSegment(bridge string) (byte, bool) {
	if !reviewedRunnerBridge(bridge) {
		return 0, false
	}
	segment, err := strconv.Atoi(strings.TrimPrefix(bridge, "vmbr"))
	if err != nil || segment < 0 || segment > 255 {
		return 0, false
	}
	return byte(segment), true
}

// runnerAddressOnBridge requires a profile's static address to sit on the
// segment its own bridge carries.
//
// validRunnerIPv4 answers a different question. It asks whether an address is
// a lab address at all, and it accepts either segment because it never sees
// which bridge the profile names. That leaves a reviewed profile able to pass
// admission with bridge vmbr8 and an address out of vmbr9's /24: a guest on
// one segment addressed for the other, which does not fail at the provisioning
// boundary but in the lab, as a runner that never answers.
func runnerAddressOnBridge(bridge, address, gateway string) bool {
	segment, reviewed := runnerBridgeSegment(bridge)
	if !reviewed || !validRunnerIPv4(address, gateway) {
		return false
	}
	prefix, err := netip.ParsePrefix(address)
	if err != nil {
		return false
	}
	return prefix.Addr().As4()[2] == segment
}

// TerraformRunnerLedger is the durable boundary shared by the runner
// provisioner and the PostgreSQL control store.
type TerraformRunnerLedger interface {
	GetRunnerVM(context.Context, string) (store.RunnerVM, error)
	GetRunnerVMOperation(context.Context, string) (store.RunnerVMOperation, error)
	RecordRunnerVMTerraformPlan(context.Context, string, string, int64, string, store.RunnerVMTerraformPlanEvidence) error
	BeginRunnerVMTerraformApply(context.Context, string, string, int64, string, string) (bool, error)
	ReadRunnerVMTerraformState(context.Context, string) ([]byte, bool, error)
	RunnerVMTerraformStateLocked(context.Context, string) (bool, error)
}

// TerraformRunnerProfile contains only reviewed, fixed guest-network settings.
// A GitHub payload can never select or alter one of these profiles. Bridge is
// checked against reviewedRunnerBridges, not against whatever guest bridges the
// Proxmox client was configured with; see that variable for why the two rules
// stay separate.
type TerraformRunnerProfile struct {
	TemplateVMID int
	Node         string
	Pool         string
	DatastoreID  string
	Bridge       string
	Cores        int
	MemoryMB     int
	IPv4Address  string
	IPv4Gateway  string
	UserName     string
}

// TerraformRunnerConfig contains operator-controlled infrastructure identity
// and one profile for each explicitly allowed disposable VMID.
type TerraformRunnerConfig struct {
	Actor             string
	ProxmoxEndpoint   string
	OpenBaoAddress    string
	OpenBaoKVMount    string
	OpenBaoSecretPath string
	Profiles          map[int]TerraformRunnerProfile
	ModuleDirectory   string
}

// TerraformRunnerProvisioner runs the fixed Terraform module and reconciles
// results against both encrypted PostgreSQL state and independent Proxmox facts.
type TerraformRunnerProvisioner struct {
	runtime  *TerraformRuntime
	ledger   TerraformRunnerLedger
	observer interface {
		Get(context.Context, proxmox.Identity) (proxmox.VM, error)
	}
	keys   *SSHAccessStore
	config TerraformRunnerConfig
}

// NewTerraformRunnerProvisioner rejects incomplete or unapproved runtime wiring.
type terraformVMProvisioner interface {
	Get(context.Context, proxmox.Identity) (proxmox.VM, error)
	Clone(context.Context, proxmox.Action, proxmox.CloneSpec) (proxmox.Result, error)
	Start(context.Context, proxmox.Action, proxmox.Identity) (proxmox.Result, error)
	Stop(context.Context, proxmox.Action, proxmox.Identity, proxmox.IdleProof) (proxmox.Result, error)
	Destroy(context.Context, proxmox.Action, proxmox.Identity, proxmox.DestroyProof) (proxmox.Result, error)
	Reconcile(context.Context, string, proxmox.Identity, string, string) (proxmox.Result, error)
}

var _ terraformVMProvisioner = (*TerraformRunnerProvisioner)(nil)

func NewTerraformRunnerProvisioner(runtime *TerraformRuntime, ledger TerraformRunnerLedger,
	observer interface {
		Get(context.Context, proxmox.Identity) (proxmox.VM, error)
	}, keys *SSHAccessStore, config TerraformRunnerConfig) (*TerraformRunnerProvisioner, error) {
	endpoint, err := validateTerraformOrigin(config.ProxmoxEndpoint)
	if runtime == nil || ledger == nil || observer == nil || keys == nil || err != nil ||
		config.Actor == "" || len(config.Actor) > 256 ||
		strings.TrimSpace(config.OpenBaoAddress) == "" ||
		config.OpenBaoKVMount == "" || config.OpenBaoSecretPath == "" ||
		!filepath.IsAbs(config.ModuleDirectory) || len(config.Profiles) == 0 {
		return nil, errors.New("invalid Terraform runner provisioner configuration")
	}
	moduleInfo, err := os.Lstat(config.ModuleDirectory)
	if err != nil || !moduleInfo.IsDir() || moduleInfo.Mode()&os.ModeSymlink != 0 {
		return nil, errors.New("fixed Terraform runner module directory is unavailable")
	}
	profiles := make(map[int]TerraformRunnerProfile, len(config.Profiles))
	for vmid, profile := range config.Profiles {
		profiles[vmid] = profile
	}
	config.Profiles = profiles
	config.ProxmoxEndpoint = endpoint
	for vmid, profile := range config.Profiles {
		if vmid < 9000 || vmid > 9099 || profile.TemplateVMID < 9000 || profile.TemplateVMID > 9099 || vmid == profile.TemplateVMID ||
			!terraformRunnerNodePattern.MatchString(profile.Node) || profile.Pool != "ra8-tf-lab" ||
			profile.DatastoreID != "ra8-tf-lab" ||
			!reviewedRunnerBridge(profile.Bridge) ||
			profile.Cores < 1 || profile.Cores > 4 ||
			profile.MemoryMB < 512 || profile.MemoryMB > 8192 ||
			profile.UserName != "ra8ci" ||
			!runnerAddressOnBridge(profile.Bridge, profile.IPv4Address, profile.IPv4Gateway) {
			return nil, errors.New("Terraform runner profile is outside the reviewed disposable policy")
		}
	}
	addresses := make(map[string]int, len(config.Profiles))
	for vmid, profile := range config.Profiles {
		if _, reservedAsTemplate := config.Profiles[profile.TemplateVMID]; reservedAsTemplate {
			return nil, errors.New("a runner VMID cannot also be configured as a clone template")
		}
		if previous, duplicate := addresses[profile.IPv4Address]; duplicate {
			return nil, fmt.Errorf("runner VMIDs %d and %d share a static address", previous, vmid)
		}
		addresses[profile.IPv4Address] = vmid
	}
	return &TerraformRunnerProvisioner{runtime: runtime, ledger: ledger,
		observer: observer, keys: keys, config: config}, nil
}

func validateTerraformOrigin(raw string) (string, error) {
	if strings.TrimSpace(raw) != raw || raw == "" {
		return "", errors.New("empty Terraform Proxmox endpoint")
	}
	// HTTPBackendEnvironment applies the same strict HTTPS-origin policy to its
	// endpoint. Reuse its parser with no credentials by validating URL fields here.
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "https" || u.Hostname() == "" || u.Port() == "" ||
		u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" ||
		personalNetworkHost(u.Hostname()) {
		return "", errors.New("Terraform Proxmox endpoint must be a reviewed HTTPS origin")
	}
	return u.String(), nil
}

func validRunnerIPv4(address, gateway string) bool {
	prefix, prefixErr := netip.ParsePrefix(address)
	gatewayAddress, gatewayErr := netip.ParseAddr(gateway)
	if prefixErr != nil || gatewayErr != nil || prefix.Bits() != 24 ||
		!prefix.Addr().Is4() || !gatewayAddress.Is4() || address != prefix.String() ||
		!prefix.Contains(gatewayAddress) {
		return false
	}
	host := prefix.Addr().As4()
	if host[0] != 10 || host[1] != 250 || (host[2] != 8 && host[2] != 9) ||
		host[3] < 2 || host[3] > 254 || gatewayAddress.As4()[3] != 1 {
		return false
	}
	return true
}

// Get provides read-only, identity-checked Proxmox observations.
func (p *TerraformRunnerProvisioner) Get(ctx context.Context, identity proxmox.Identity) (proxmox.VM, error) {
	if p == nil || p.observer == nil || ctx == nil || !p.validIdentity(identity) {
		return proxmox.VM{}, proxmox.ErrInvalid
	}
	return p.observer.Get(ctx, identity)
}

// Clone creates one immutable Terraform plan and consumes its one-way apply intent.
func (p *TerraformRunnerProvisioner) Clone(ctx context.Context, action proxmox.Action, spec proxmox.CloneSpec) (proxmox.Result, error) {
	if spec.Target.CreationOperationID != action.ID || spec.Target.VMID == spec.TemplateVMID {
		return proxmox.Result{}, proxmox.ErrInvalid
	}
	vm, err := p.reservation(ctx, spec.Target)
	if err != nil || vm.TemplateVMID != spec.TemplateVMID ||
		vm.TemplateName != spec.TemplateName || vm.TemplateDigest != spec.TemplateDigest {
		return proxmox.Result{}, proxmox.ErrConflict
	}
	// Last before the apply intent is consumed, and the only observation in
	// this method: the other three lifecycle steps each read Proxmox and
	// refuse on what they see, and a clone into an occupied VMID has to be
	// refused here rather than found by Terraform with the intent spent.
	if err := checkCloneTargetIsFree(ctx, p.Get, spec.Target); err != nil {
		return proxmox.Result{}, err
	}
	return p.apply(ctx, action, vm, "clone", false, false)
}

// Start connects only the isolated, reserved runner NIC while powering up.
func (p *TerraformRunnerProvisioner) Start(ctx context.Context, action proxmox.Action, identity proxmox.Identity) (proxmox.Result, error) {
	vm, err := p.reservation(ctx, identity)
	if err != nil {
		return proxmox.Result{}, err
	}
	observed, err := p.Get(ctx, identity)
	if err != nil || observed.Status != "stopped" || observed.Locked || observed.Protected {
		return proxmox.Result{}, errors.New("runner must be stopped, unlocked, and unprotected before start")
	}
	return p.apply(ctx, action, vm, "start", true, true)
}

// Stop requires fresh cooperative-drain evidence before disconnecting and stopping.
func (p *TerraformRunnerProvisioner) Stop(ctx context.Context, action proxmox.Action, identity proxmox.Identity, proof proxmox.IdleProof) (proxmox.Result, error) {
	now := time.Now()
	if proof.VMID != identity.VMID || proof.ReservationID != identity.ReservationID ||
		!store.ValidID(proof.EvidenceID) || !proof.Drained || !proof.NoActiveJob ||
		proof.ObservedAt.IsZero() || proof.ObservedAt.After(now.Add(time.Second)) ||
		now.Sub(proof.ObservedAt) > 10*time.Second {
		return proxmox.Result{}, errors.New("Terraform stop requires fresh cooperative drain evidence")
	}
	vm, err := p.reservation(ctx, identity)
	if err != nil {
		return proxmox.Result{}, err
	}
	observed, err := p.Get(ctx, identity)
	if err != nil || observed.Status != "running" || observed.Locked || observed.Protected {
		return proxmox.Result{}, errors.New("runner must be running, unlocked, and unprotected before stop")
	}
	return p.apply(ctx, action, vm, "stop", false, false)
}

// Destroy requires deregistration, state reconciliation, idle proof, and separate approval.
func (p *TerraformRunnerProvisioner) Destroy(ctx context.Context, action proxmox.Action, identity proxmox.Identity, proof proxmox.DestroyProof) (proxmox.Result, error) {
	if !validTerraformIdleProof(identity, proof.IdleProof) || !store.ValidID(proof.ApprovalID) ||
		!proof.RunnerDeregistered || !proof.StateReconciled ||
		!terraformRunnerConfigDigestPattern.MatchString(proof.ExpectedConfigDigest) {
		return proxmox.Result{}, errors.New("Terraform destroy requires reviewed runner cleanup evidence")
	}
	observed, err := p.Get(ctx, identity)
	if err != nil || observed.Status != "stopped" || observed.Protected || observed.Locked || observed.ConfigDigest != proof.ExpectedConfigDigest {
		return proxmox.Result{}, errors.New("runner identity or configuration changed before Terraform destroy")
	}
	vm, err := p.reservation(ctx, identity)
	if err != nil {
		return proxmox.Result{}, err
	}
	return p.apply(ctx, action, vm, "destroy", false, false)
}

func validTerraformIdleProof(identity proxmox.Identity, proof proxmox.IdleProof) bool {
	now := time.Now()
	return proof.VMID == identity.VMID && proof.ReservationID == identity.ReservationID &&
		store.ValidID(proof.EvidenceID) && proof.Drained && proof.NoActiveJob &&
		!proof.ObservedAt.IsZero() && !proof.ObservedAt.After(now.Add(time.Second)) &&
		now.Sub(proof.ObservedAt) <= 10*time.Second
}

func (p *TerraformRunnerProvisioner) reservation(ctx context.Context, identity proxmox.Identity) (store.RunnerVM, error) {
	if p == nil || ctx == nil || !p.validIdentity(identity) {
		return store.RunnerVM{}, proxmox.ErrInvalid
	}
	vm, err := p.ledger.GetRunnerVM(ctx, identity.ReservationID)
	if err != nil {
		return store.RunnerVM{}, err
	}
	if vm.ID != identity.ReservationID || vm.VMID != identity.VMID || vm.Node != identity.Node ||
		vm.Pool != identity.Pool || vm.Storage != identity.Storage || vm.Name != identity.Name ||
		vm.CreationOperationID != identity.CreationOperationID {
		return store.RunnerVM{}, proxmox.ErrConflict
	}
	profile, ok := p.config.Profiles[vm.VMID]
	if !ok || profile.Node != vm.Node || profile.Pool != vm.Pool || profile.DatastoreID != vm.Storage ||
		profile.TemplateVMID != vm.TemplateVMID {
		return store.RunnerVM{}, errors.New("durable runner reservation differs from its fixed Terraform profile")
	}
	return vm, nil
}

func (p *TerraformRunnerProvisioner) validIdentity(identity proxmox.Identity) bool {
	profile, ok := p.config.Profiles[identity.VMID]
	return ok && identity.VMID >= 9000 && identity.VMID <= 9099 &&
		identity.Name == fmt.Sprintf("ra8-lab-ci-%d", identity.VMID) && identity.Node == profile.Node &&
		identity.Pool == profile.Pool && identity.Storage == profile.DatastoreID &&
		store.ValidID(identity.ReservationID) && store.ValidID(identity.CreationOperationID) &&
		len(identity.Name) > 0 && len(identity.Name) <= 64
}

func (p *TerraformRunnerProvisioner) apply(ctx context.Context, action proxmox.Action,
	vm store.RunnerVM, kind string, started, networkEnabled bool) (proxmox.Result, error) {
	if ctx == nil || action.PriorRequestIssued || !store.ValidID(action.ID) ||
		vm.CurrentOperationID != action.ID || !vm.UnknownOutcome {
		return proxmox.Result{}, &proxmox.UnknownOutcomeError{OperationID: action.ID, Cause: errors.New("operation is not a new durable intent")}
	}
	op, err := p.ledger.GetRunnerVMOperation(ctx, action.ID)
	if err != nil {
		return proxmox.Result{}, err
	}
	if op.RunnerVMID != vm.ID || op.Kind != kind || op.Status != "unresolved" ||
		op.ProviderKind != "proxmox" || op.UPID != "" {
		return proxmox.Result{}, store.ErrConflict
	}
	var key SSHAccessKey
	if kind == "clone" {
		key, err = p.keys.Ensure(vm.ID)
	} else {
		key, err = p.keys.Load(vm.ID)
	}
	if err != nil {
		return proxmox.Result{}, err
	}
	profile := p.config.Profiles[vm.VMID]
	variables := map[string]any{
		"proxmox_endpoint":    p.config.ProxmoxEndpoint,
		"openbao_address":     p.config.OpenBaoAddress,
		"openbao_kv_mount":    p.config.OpenBaoKVMount,
		"openbao_secret_path": p.config.OpenBaoSecretPath,
		"runner": map[string]any{
			"reservation_id":        vm.ID,
			"creation_operation_id": vm.CreationOperationID,
			"run_id":                fmt.Sprintf("%016x", uint64(vm.WorkflowRunID)),
			"vm_id":                 vm.VMID,
			"template_vm_id":        profile.TemplateVMID,
			"node_name":             profile.Node,
			"pool_id":               profile.Pool,
			"datastore_id":          profile.DatastoreID,
			"bridge":                profile.Bridge,
			"cores":                 profile.Cores,
			"memory_mb":             profile.MemoryMB,
			"ipv4_address":          profile.IPv4Address,
			"ipv4_gateway":          profile.IPv4Gateway,
			"ssh_public_keys":       []string{key.PublicKey},
			"user_name":             profile.UserName,
			"started":               started,
			"network_enabled":       networkEnabled,
		},
	}
	input, err := json.Marshal(variables)
	if err != nil {
		return proxmox.Result{}, errors.New("encode fixed Terraform runner inputs")
	}
	defer clear(input)
	stateIdentity := terraformRunnerStateIdentity(vm.ID)
	err = p.runtime.WithSession(ctx, vm.ID, func(session *TerraformSession) error {
		if err := session.Init(ctx); err != nil {
			return err
		}
		variableFile, err := writeTerraformVariables(session.workspace, action.ID, input)
		if err != nil {
			return err
		}
		moduleSHA, err := terraformModuleDigest(p.runtime.config.EnvironmentDirectory, p.config.ModuleDirectory)
		if err != nil {
			return err
		}
		lockSHA, err := fileSHA256(filepath.Join(p.runtime.config.EnvironmentDirectory, ".terraform.lock.hcl"), 1<<20)
		if err != nil {
			return errors.New("fixed Terraform provider lockfile is unavailable")
		}
		inputSHA, err := fileSHA256(variableFile, 1<<20)
		if err != nil {
			return err
		}
		planFile, digest, err := session.Plan(ctx, action.ID, variableFile, kind == "destroy")
		if err != nil {
			return err
		}
		evidence := store.RunnerVMTerraformPlanEvidence{
			TerraformVersion: p.runtime.config.Version, PlanSHA256: digest,
			ModuleSHA256: moduleSHA, InputSHA256: inputSHA,
			ProviderLockSHA256: lockSHA, StateIdentitySHA256: stateIdentity,
			PreparedAt: time.Now().UTC(),
		}
		if err := p.ledger.RecordRunnerVMTerraformPlan(ctx, p.config.Actor, vm.ID,
			op.Generation, action.ID, evidence); err != nil {
			return err
		}
		begun, err := p.ledger.BeginRunnerVMTerraformApply(ctx, p.config.Actor,
			vm.ID, op.Generation, action.ID, digest)
		if err != nil {
			return err
		}
		if !begun {
			return errors.New("Terraform apply intent was already consumed; reconcile without retry")
		}
		return session.Apply(ctx, planFile, digest)
	})
	if err != nil {
		return proxmox.Result{}, &proxmox.UnknownOutcomeError{OperationID: action.ID, Cause: err}
	}
	return p.Reconcile(ctx, action.ID, proxmox.Identity{
		VMID: vm.VMID, Node: vm.Node, Pool: vm.Pool, Storage: vm.Storage,
		Name: vm.Name, ReservationID: vm.ID, CreationOperationID: vm.CreationOperationID,
	}, kind, "")
}

// Reconcile never repeats Terraform Apply. It requires the one-way apply intent
// plus unlocked encrypted state and a matching independent Proxmox observation.
func (p *TerraformRunnerProvisioner) Reconcile(ctx context.Context, operationID string,
	identity proxmox.Identity, kind, upid string) (proxmox.Result, error) {
	if ctx == nil || upid != "" || !store.ValidID(operationID) {
		return proxmox.Result{}, errors.New("invalid Terraform reconciliation request")
	}
	vm, err := p.reservation(ctx, identity)
	if err != nil {
		return proxmox.Result{}, err
	}
	op, err := p.ledger.GetRunnerVMOperation(ctx, operationID)
	if err != nil {
		return proxmox.Result{}, err
	}
	if op.RunnerVMID != vm.ID || op.Kind != kind || op.Status != "unresolved" {
		return proxmox.Result{}, store.ErrConflict
	}
	if op.TerraformApplyStartedAt == nil {
		if op.ProviderKind == "proxmox" && op.PlanSHA256 == "" {
			return proxmox.Result{TerraformPreflightNoEffect: &proxmox.TerraformPreflightNoEffect{ObservedAt: time.Now()}}, nil
		}
		if op.ProviderKind == "terraform" && op.PlanSHA256 != "" &&
			op.StateIdentitySHA256 == terraformRunnerStateIdentity(vm.ID) {
			return proxmox.Result{TerraformPreflightNoEffect: &proxmox.TerraformPreflightNoEffect{
				PlanSHA256: op.PlanSHA256, StateIdentitySHA256: op.StateIdentitySHA256, ObservedAt: time.Now()}}, nil
		}
		return proxmox.Result{}, errors.New("Terraform apply status is not safely reconcilable")
	}
	if op.ProviderKind != "terraform" || op.PlanSHA256 == "" ||
		op.StateIdentitySHA256 != terraformRunnerStateIdentity(vm.ID) {
		return proxmox.Result{}, errors.New("Terraform operation evidence differs from this reservation")
	}
	locked, err := p.ledger.RunnerVMTerraformStateLocked(ctx, vm.ID)
	if err != nil || locked {
		if err != nil {
			return proxmox.Result{}, err
		}
		return proxmox.Result{}, errors.New("Terraform state lock is still held; outcome remains unknown")
	}
	state, hasState, err := p.ledger.ReadRunnerVMTerraformState(ctx, vm.ID)
	if err != nil {
		return proxmox.Result{}, err
	}
	stateHash := sha256.Sum256([]byte("ra8ci-no-state"))
	hasRunner := false
	if hasState {
		stateHash = sha256.Sum256(state)
		hasRunner, err = terraformStateHasRunner(state, vm)
		clear(state)
		if err != nil {
			return proxmox.Result{}, err
		}
	}
	observed, observeErr := p.observer.Get(ctx, identity)
	absent := errors.Is(observeErr, proxmox.ErrNotFound)
	if observeErr != nil && !absent {
		return proxmox.Result{}, observeErr
	}
	if !absent && observed.Locked {
		return proxmox.Result{}, errors.New("Proxmox guest remains locked; outcome is not yet reconcilable")
	}
	outcome, err := terraformLifecycleOutcome(kind, hasRunner, absent, observed.Status)
	if err != nil {
		return proxmox.Result{}, err
	}
	if kind == "destroy" && outcome == "succeeded" {
		if err := p.keys.Remove(vm.ID); err != nil {
			return proxmox.Result{}, err
		}
	}
	return proxmox.Result{TerraformEvidence: &proxmox.TerraformEvidence{
		Outcome: outcome, PlanSHA256: op.PlanSHA256,
		StateIdentitySHA256:  op.StateIdentitySHA256,
		ReconciliationSHA256: hex.EncodeToString(stateHash[:]),
		StateHasVM:           hasRunner, VMAbsent: absent,
		VMStatus: func() string {
			if absent {
				return ""
			}
			return observed.Status
		}(),
		ObservedAt: time.Now(),
	}}, nil
}

func terraformLifecycleOutcome(kind string, hasStateRunner, vmAbsent bool, vmStatus string) (string, error) {
	switch kind {
	case "clone":
		if hasStateRunner && !vmAbsent && vmStatus == "stopped" {
			return "succeeded", nil
		}
		if !hasStateRunner && vmAbsent {
			return "failed", nil
		}
	case "start":
		if hasStateRunner && !vmAbsent && vmStatus == "running" {
			return "succeeded", nil
		}
		if hasStateRunner && !vmAbsent && vmStatus == "stopped" {
			return "failed", nil
		}
	case "stop":
		if hasStateRunner && !vmAbsent && vmStatus == "stopped" {
			return "succeeded", nil
		}
		if hasStateRunner && !vmAbsent && vmStatus == "running" {
			return "failed", nil
		}
	case "destroy":
		if !hasStateRunner && vmAbsent {
			return "succeeded", nil
		}
		if hasStateRunner && !vmAbsent && vmStatus == "stopped" {
			return "failed", nil
		}
	}
	return "", errors.New("Terraform state and independent Proxmox observation are inconsistent")
}

func terraformRunnerStateIdentity(reservationID string) string {
	digest := sha256.Sum256([]byte("ra8ci-runner-terraform-state:" + reservationID))
	return hex.EncodeToString(digest[:])
}

func writeTerraformVariables(workspace, operationID string, body []byte) (string, error) {
	if !store.ValidID(operationID) {
		return "", errors.New("invalid Terraform operation input")
	}
	directory := filepath.Join(workspace, operationID)
	if err := secureDirectory(directory); err != nil {
		return "", err
	}
	file := filepath.Join(directory, "runner.tfvars.json")
	handle, err := os.OpenFile(file, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o600)
	if err != nil {
		return "", errors.New("create private Terraform runner input")
	}
	written, writeErr := handle.Write(body)
	syncErr := handle.Sync()
	closeErr := handle.Close()
	if writeErr != nil || syncErr != nil || closeErr != nil || written != len(body) {
		_ = os.Remove(file)
		return "", errors.New("persist private Terraform runner input")
	}
	return file, nil
}

func terraformModuleDigest(environment, module string) (string, error) {
	envDigest, err := digestTerraformTree(environment)
	if err != nil {
		return "", err
	}
	moduleDigest, err := digestTerraformTree(module)
	if err != nil {
		return "", err
	}
	hash := sha256.Sum256([]byte(envDigest + ":" + moduleDigest))
	return hex.EncodeToString(hash[:]), nil
}

func digestTerraformTree(root string) (string, error) {
	root, err := filepath.Abs(root)
	if err != nil {
		return "", err
	}
	var files []string
	err = filepath.WalkDir(root, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path != root && entry.IsDir() && entry.Name() == ".terraform" {
			return filepath.SkipDir
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return errors.New("Terraform source tree contains a symlink")
		}
		if !entry.IsDir() {
			if !entry.Type().IsRegular() {
				return errors.New("Terraform source tree contains a non-regular file")
			}
			files = append(files, path)
		}
		return nil
	})
	if err != nil || len(files) == 0 || len(files) > 4096 {
		return "", errors.New("Terraform source tree is unavailable or unbounded")
	}
	sort.Strings(files)
	hash := sha256.New()
	var total int64
	for _, file := range files {
		relative, err := filepath.Rel(root, file)
		if err != nil {
			return "", err
		}
		info, err := os.Stat(file)
		if err != nil || total > 64<<20-info.Size() {
			return "", errors.New("Terraform source tree exceeds its size bound")
		}
		total += info.Size()
		if _, err := io.WriteString(hash, relative+"\x00"); err != nil {
			return "", err
		}
		handle, err := os.Open(file)
		if err != nil {
			return "", errors.New("Terraform source file cannot be opened")
		}
		_, copyErr := io.Copy(hash, io.LimitReader(handle, info.Size()+1))
		closeErr := handle.Close()
		if copyErr != nil || closeErr != nil {
			return "", errors.New("Terraform source digest failed")
		}
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func terraformStateHasRunner(body []byte, vm store.RunnerVM) (bool, error) {
	if len(body) == 0 || len(body) > maxTerraformStateBytes {
		return false, errors.New("Terraform state is empty or exceeds its bound")
	}
	var state map[string]json.RawMessage
	if err := json.Unmarshal(body, &state); err != nil {
		return false, errors.New("Terraform state is malformed")
	}
	var version int
	var serial int64
	var lineage, terraformVersion string
	if json.Unmarshal(state["version"], &version) != nil ||
		json.Unmarshal(state["serial"], &serial) != nil ||
		json.Unmarshal(state["lineage"], &lineage) != nil ||
		json.Unmarshal(state["terraform_version"], &terraformVersion) != nil ||
		version != 4 || serial < 0 || !terraformRunnerLineagePattern.MatchString(lineage) ||
		!terraformRunnerVersionPattern.MatchString(terraformVersion) {
		return false, errors.New("Terraform state identity is malformed")
	}
	count := 0
	var visit func(map[string]json.RawMessage) error
	visit = func(module map[string]json.RawMessage) error {
		var resources []json.RawMessage
		if raw := module["resources"]; len(raw) > 0 {
			if err := json.Unmarshal(raw, &resources); err != nil {
				return errors.New("Terraform resource state is malformed")
			}
		}
		for _, raw := range resources {
			var resource map[string]json.RawMessage
			if json.Unmarshal(raw, &resource) != nil {
				return errors.New("Terraform resource state is malformed")
			}
			var resourceType, resourceName string
			_ = json.Unmarshal(resource["type"], &resourceType)
			_ = json.Unmarshal(resource["name"], &resourceName)
			if resourceType != "proxmox_virtual_environment_vm" || resourceName != "runner" {
				continue
			}
			var instances []json.RawMessage
			if json.Unmarshal(resource["instances"], &instances) != nil || len(instances) != 1 {
				return errors.New("Terraform runner resource instance count is invalid")
			}
			var instance map[string]json.RawMessage
			if json.Unmarshal(instances[0], &instance) != nil {
				return errors.New("Terraform runner resource instance is malformed")
			}
			var attributes map[string]json.RawMessage
			if json.Unmarshal(instance["attributes"], &attributes) != nil {
				return errors.New("Terraform runner resource attributes are malformed")
			}
			var vmid int
			var name, description string
			vmRaw := attributes["vm_id"]
			if len(vmRaw) == 0 {
				vmRaw = attributes["vmid"]
			}
			if json.Unmarshal(vmRaw, &vmid) != nil ||
				json.Unmarshal(attributes["name"], &name) != nil ||
				json.Unmarshal(attributes["description"], &description) != nil ||
				vmid != vm.VMID || name != vm.Name ||
				description != "RA8CI_RESERVATION="+vm.ID+";RA8CI_OPERATION="+vm.CreationOperationID {
				return errors.New("Terraform state runner identity differs from reservation")
			}
			count++
		}
		var children []json.RawMessage
		if raw := module["child_modules"]; len(raw) > 0 {
			if json.Unmarshal(raw, &children) != nil {
				return errors.New("Terraform child module state is malformed")
			}
		}
		for _, raw := range children {
			var child map[string]json.RawMessage
			if json.Unmarshal(raw, &child) != nil {
				return errors.New("Terraform child module state is malformed")
			}
			if err := visit(child); err != nil {
				return err
			}
		}
		return nil
	}
	if err := visit(state); err != nil {
		return false, err
	}
	if count > 1 {
		return false, errors.New("Terraform state contains duplicate runner resources")
	}
	return count == 1, nil
}
