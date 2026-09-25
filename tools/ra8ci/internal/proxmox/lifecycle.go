// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

var (
	diskKeyPattern = regexp.MustCompile(`^(scsi|virtio|sata)[0-9]+$`)
	netKeyPattern  = regexp.MustCompile(`^net[0-9]+$`)
	digestPattern  = regexp.MustCompile(`^[0-9a-f]{40}$`)
)

// Identity is the exact VM reservation. The creation operation marker is
// written through the clone API and must match on every later observation.
type Identity struct {
	VMID                int
	Node                string
	Pool                string
	Storage             string
	Name                string
	ReservationID       string
	CreationOperationID string
}

func (i Identity) marker() string {
	return "RA8CI_RESERVATION=" + i.ReservationID + ";RA8CI_OPERATION=" + i.CreationOperationID
}

func (c *Client) validateIdentity(i Identity) error {
	if _, ok := c.allowedVMIDs[i.VMID]; !ok || i.VMID < 9000 || i.Node != c.node || i.Pool != c.pool || i.Storage != c.storage || !namePattern.MatchString(i.Name) || !idPattern.MatchString(i.ReservationID) || !idPattern.MatchString(i.CreationOperationID) {
		return fmt.Errorf("%w: VM reservation outside approved identity", ErrInvalid)
	}
	return nil
}

// Action describes one durable external-operation intent. The caller must
// persist it before issuing a request; a prior attempt must be reconciled.
type Action struct {
	ID                 string
	PriorRequestIssued bool
}

func validateAction(a Action) error {
	if !idPattern.MatchString(a.ID) {
		return fmt.Errorf("%w: operation ID must be a canonical UUID", ErrInvalid)
	}
	if a.PriorRequestIssued {
		return &UnknownOutcomeError{OperationID: a.ID, Cause: errors.New("prior request requires reconciliation")}
	}
	return nil
}

// UnknownOutcomeError never authorizes a retry. UPID is populated when the
// API accepted a mutation but its eventual result could not be verified.
type UnknownOutcomeError struct {
	OperationID string
	UPID        string
	Cause       error
}

func (e *UnknownOutcomeError) Error() string {
	return fmt.Sprintf("%s (operation %s, task %s): %v", ErrUnknownOutcome, e.OperationID, e.UPID, e.Cause)
}

func (e *UnknownOutcomeError) Unwrap() error { return ErrUnknownOutcome }

type resource struct {
	VMID     int             `json:"vmid"`
	Type     string          `json:"type"`
	Node     string          `json:"node"`
	Name     string          `json:"name"`
	Pool     string          `json:"pool"`
	Status   string          `json:"status"`
	Template json.RawMessage `json:"template"`
}

// VM is a verified observation, not proof that a particular request caused it.
type VM struct {
	Identity     Identity
	Status       string
	ConfigDigest string
	Protected    bool
	Locked       bool
}

// Result distinguishes an accepted/verified task from an already-satisfied
// state; callers must not record the latter as an executed external action.
type TerraformEvidence struct {
	Outcome              string
	PlanSHA256           string
	StateIdentitySHA256  string
	ReconciliationSHA256 string
	StateHasVM           bool
	VMAbsent             bool
	VMStatus             string
	ObservedAt           time.Time
}

// TerraformPreflightNoEffect proves no durable apply intent was committed.
type TerraformPreflightNoEffect struct {
	PlanSHA256          string
	StateIdentitySHA256 string
	ObservedAt          time.Time
}

type Result struct {
	VM                         *VM
	UPID                       string
	TerraformEvidence          *TerraformEvidence
	TerraformPreflightNoEffect *TerraformPreflightNoEffect
	AlreadySatisfied           bool
}

// List reports only explicitly allowlisted IDs in the configured pool.
func (c *Client) List(ctx context.Context) ([]VM, error) {
	resources, err := c.resources(ctx)
	if err != nil {
		return nil, err
	}
	result := make([]VM, 0, len(c.allowedVMIDs))
	for _, r := range resources {
		if _, ok := c.allowedVMIDs[r.VMID]; !ok {
			continue
		}
		if r.Type != "qemu" {
			return nil, fmt.Errorf("%w: allowed VM ID is not QEMU", ErrConflict)
		}
		if r.Pool != c.pool {
			return nil, fmt.Errorf("%w: allowed VM ID is in a different pool", ErrConflict)
		}
		result = append(result, VM{Identity: Identity{VMID: r.VMID, Node: r.Node, Pool: r.Pool, Name: r.Name}, Status: r.Status})
	}
	sort.Slice(result, func(a, b int) bool { return result[a].Identity.VMID < result[b].Identity.VMID })
	return result, nil
}

func (c *Client) resources(ctx context.Context) (map[int]resource, error) {
	var entries []resource
	if _, err := c.request(ctx, http.MethodGet, "/cluster/resources?type=vm", nil, &entries); err != nil {
		return nil, err
	}
	result := make(map[int]resource, len(entries))
	for _, entry := range entries {
		if entry.VMID < 100 || entry.VMID > 999999999 || entry.Type == "" {
			return nil, ErrProtocol
		}
		if _, duplicate := result[entry.VMID]; duplicate {
			return nil, fmt.Errorf("%w: duplicate cluster VM ID", ErrProtocol)
		}
		result[entry.VMID] = entry
	}
	return result, nil
}

// Get rechecks node, pool, name, creation marker, disk storage, and VM state.
func (c *Client) Get(ctx context.Context, identity Identity) (VM, error) {
	if err := c.validateIdentity(identity); err != nil {
		return VM{}, err
	}
	resources, err := c.resources(ctx)
	if err != nil {
		return VM{}, err
	}
	r, exists := resources[identity.VMID]
	if !exists {
		return VM{}, ErrNotFound
	}
	return c.inspect(ctx, identity, r)
}

func (c *Client) inspect(ctx context.Context, identity Identity, r resource) (VM, error) {
	resourceTemplate, flagErr := flagRaw(r.Template)
	if flagErr != nil || r.Type != "qemu" || r.Node != identity.Node || r.Pool != identity.Pool || r.Name != identity.Name || resourceTemplate {
		return VM{}, fmt.Errorf("%w: cluster identity differs from reservation", ErrConflict)
	}
	var config map[string]json.RawMessage
	if _, err := c.request(ctx, http.MethodGet, vmPath(identity.Node, identity.VMID)+"/config", nil, &config); err != nil {
		return VM{}, err
	}
	description, err := stringField(config, "description")
	if err != nil || description != identity.marker() {
		return VM{}, fmt.Errorf("%w: reservation marker mismatch", ErrConflict)
	}
	name, err := stringField(config, "name")
	if err != nil || name != identity.Name {
		return VM{}, fmt.Errorf("%w: configuration name mismatch", ErrConflict)
	}
	digest, err := stringField(config, "digest")
	if err != nil || !digestPattern.MatchString(digest) {
		return VM{}, ErrProtocol
	}
	protected, err := boolField(config, "protection")
	if err != nil {
		return VM{}, ErrProtocol
	}
	template, err := boolField(config, "template")
	if err != nil || template {
		return VM{}, fmt.Errorf("%w: target became a template", ErrConflict)
	}
	lock, err := optionalStringField(config, "lock")
	if err != nil {
		return VM{}, ErrProtocol
	}
	storageFound := false
	for key, raw := range config {
		if !diskKeyPattern.MatchString(key) {
			continue
		}
		var value string
		if err := json.Unmarshal(raw, &value); err != nil {
			return VM{}, ErrProtocol
		}
		if strings.HasPrefix(value, identity.Storage+":") {
			storageFound = true
		}
	}
	if !storageFound {
		return VM{}, fmt.Errorf("%w: no disk on approved storage", ErrConflict)
	}
	if err := c.checkNetworks(config, "reservation"); err != nil {
		return VM{}, err
	}
	var state struct {
		VMID   int    `json:"vmid"`
		Status string `json:"status"`
	}
	if _, err := c.request(ctx, http.MethodGet, vmPath(identity.Node, identity.VMID)+"/status/current", nil, &state); err != nil {
		return VM{}, err
	}
	if state.VMID != identity.VMID || (state.Status != "running" && state.Status != "stopped") || (r.Status != "" && r.Status != state.Status) {
		return VM{}, fmt.Errorf("%w: inconsistent VM status", ErrProtocol)
	}
	return VM{Identity: identity, Status: state.Status, ConfigDigest: digest, Protected: protected, Locked: lock != ""}, nil
}

// checkNetworks requires every interface a guest carries to sit on a reviewed
// bridge, not merely one of them: a second interface on the management bridge
// is exactly the escape the pool, storage, and marker checks cannot see. An
// interface with no bridge= setting is refused rather than read as harmless,
// because a passed-through device is not a bridge this client can approve.
func (c *Client) checkNetworks(config map[string]json.RawMessage, subject string) error {
	found := false
	for key, raw := range config {
		if !netKeyPattern.MatchString(key) {
			continue
		}
		var value string
		if err := json.Unmarshal(raw, &value); err != nil {
			return ErrProtocol
		}
		bridge, ok := bridgeOf(value)
		if !ok {
			return fmt.Errorf("%w: %s interface %s declares no bridge", ErrConflict, subject, key)
		}
		if _, approved := c.bridges[bridge]; !approved {
			return fmt.Errorf("%w: %s interface %s is on unreviewed bridge %q", ErrConflict, subject, key, bridge)
		}
		found = true
	}
	if !found {
		return fmt.Errorf("%w: %s has no interface on an approved bridge", ErrConflict, subject)
	}
	return nil
}

// bridgeOf reads the bridge out of a Proxmox net line such as
// "virtio=AA:BB:CC:DD:EE:FF,bridge=vmbr8,firewall=1".
func bridgeOf(value string) (string, bool) {
	for _, field := range strings.Split(value, ",") {
		name, setting, ok := strings.Cut(field, "=")
		if ok && name == "bridge" {
			return setting, setting != ""
		}
	}
	return "", false
}

func stringField(values map[string]json.RawMessage, key string) (string, error) {
	raw, ok := values[key]
	if !ok {
		return "", ErrProtocol
	}
	var value string
	if err := json.Unmarshal(raw, &value); err != nil {
		return "", ErrProtocol
	}
	return value, nil
}

func optionalStringField(values map[string]json.RawMessage, key string) (string, error) {
	if _, ok := values[key]; !ok {
		return "", nil
	}
	return stringField(values, key)
}

func boolField(values map[string]json.RawMessage, key string) (bool, error) {
	raw, ok := values[key]
	if !ok {
		return false, nil
	}
	return flagRaw(raw)
}

func flagRaw(raw json.RawMessage) (bool, error) {
	if len(raw) == 0 {
		return false, nil
	}
	switch string(raw) {
	case "true", "1", `"1"`:
		return true, nil
	case "false", "0", `"0"`:
		return false, nil
	default:
		return false, ErrProtocol
	}
}

// CloneSpec selects an exact pre-reviewed source template, never an image URL.
type CloneSpec struct {
	Target         Identity
	TemplateVMID   int
	TemplateName   string
	TemplateDigest string
}

// Clone sends at most one full-clone request for a durable operation intent.
// An existing exact marker is an idempotent observation, not a second clone.
func (c *Client) Clone(ctx context.Context, action Action, spec CloneSpec) (Result, error) {
	if err := c.validateIdentity(spec.Target); err != nil {
		return Result{}, err
	}
	if err := validateAction(action); err != nil {
		return Result{}, err
	}
	if action.ID != spec.Target.CreationOperationID {
		return Result{}, fmt.Errorf("%w: clone operation does not match the reservation creation marker", ErrInvalid)
	}
	if _, ok := c.templateVMIDs[spec.TemplateVMID]; !ok || spec.TemplateVMID == spec.Target.VMID || !namePattern.MatchString(spec.TemplateName) || !digestPattern.MatchString(spec.TemplateDigest) {
		return Result{}, fmt.Errorf("%w: source template not approved", ErrInvalid)
	}
	opCtx, cancel := context.WithTimeout(ctx, c.operationTimeout)
	defer cancel()
	resources, err := c.resources(opCtx)
	if err != nil {
		return Result{}, err
	}
	if existing, ok := resources[spec.Target.VMID]; ok {
		vm, err := c.inspect(opCtx, spec.Target, existing)
		if err != nil {
			return Result{}, err
		}
		if vm.Locked {
			return Result{}, &UnknownOutcomeError{OperationID: action.ID, Cause: ErrConflict}
		}
		return Result{VM: &vm, AlreadySatisfied: true}, nil
	}
	template, ok := resources[spec.TemplateVMID]
	templateFlag, flagErr := flagRaw(template.Template)
	if !ok || flagErr != nil || template.Type != "qemu" || template.Node != c.node || template.Name != spec.TemplateName || !templateFlag || template.Status != "stopped" {
		return Result{}, fmt.Errorf("%w: source template identity mismatch", ErrConflict)
	}
	var sourceConfig map[string]json.RawMessage
	if _, err := c.request(opCtx, http.MethodGet, vmPath(c.node, spec.TemplateVMID)+"/config", nil, &sourceConfig); err != nil {
		return Result{}, err
	}
	sourceDigest, digestErr := stringField(sourceConfig, "digest")
	sourceName, nameErr := stringField(sourceConfig, "name")
	sourceTemplate, templateErr := boolField(sourceConfig, "template")
	if digestErr != nil || nameErr != nil || templateErr != nil || sourceDigest != spec.TemplateDigest || sourceName != spec.TemplateName || !sourceTemplate {
		return Result{}, fmt.Errorf("%w: reviewed source template digest or identity changed", ErrConflict)
	}
	if err := c.checkNetworks(sourceConfig, "source template"); err != nil {
		return Result{}, err
	}
	form := url.Values{
		"newid":       {strconv.Itoa(spec.Target.VMID)},
		"name":        {spec.Target.Name},
		"description": {spec.Target.marker()},
		"full":        {"1"},
		"pool":        {c.pool},
		"storage":     {c.storage},
	}
	return c.mutateAndVerify(opCtx, action, http.MethodPost, vmPath(c.node, spec.TemplateVMID)+"/clone", form, spec.Target, "clone")
}

// Start requires an exact existing reservation and sends no force/skiplock flag.
func (c *Client) Start(ctx context.Context, action Action, identity Identity) (Result, error) {
	if err := validateAction(action); err != nil {
		return Result{}, err
	}
	opCtx, cancel := context.WithTimeout(ctx, c.operationTimeout)
	defer cancel()
	vm, err := c.Get(opCtx, identity)
	if err != nil || vm.Locked {
		if err != nil {
			return Result{}, err
		}
		return Result{}, ErrConflict
	}
	if vm.Status == "running" {
		return Result{VM: &vm, AlreadySatisfied: true}, nil
	}
	return c.mutateAndVerify(opCtx, action, http.MethodPost, vmPath(c.node, identity.VMID)+"/status/start", url.Values{}, identity, "start")
}

// IdleProof must come from a durable drain/capacity decision, not a guest.
// The client checks freshness but cannot prove GitHub will not assign a job.
type IdleProof struct {
	VMID          int
	ReservationID string
	EvidenceID    string
	ObservedAt    time.Time
	Drained       bool
	NoActiveJob   bool
}

func validateIdleProof(identity Identity, proof IdleProof, now time.Time) error {
	if proof.VMID != identity.VMID || proof.ReservationID != identity.ReservationID || !idPattern.MatchString(proof.EvidenceID) || !proof.Drained || !proof.NoActiveJob || proof.ObservedAt.IsZero() || proof.ObservedAt.After(now.Add(time.Second)) || now.Sub(proof.ObservedAt) > 10*time.Second {
		return fmt.Errorf("%w: fresh durable drain and idle evidence required", ErrInvalid)
	}
	return nil
}

// Stop is a hard VM stop. The caller must first drain GitHub capacity and
// verify no Runner.Worker/job is active; this method never assumes idle.
func (c *Client) Stop(ctx context.Context, action Action, identity Identity, proof IdleProof) (Result, error) {
	if err := validateAction(action); err != nil {
		return Result{}, err
	}
	if err := validateIdleProof(identity, proof, time.Now()); err != nil {
		return Result{}, err
	}
	opCtx, cancel := context.WithTimeout(ctx, c.operationTimeout)
	defer cancel()
	vm, err := c.Get(opCtx, identity)
	if err != nil || vm.Locked {
		if err != nil {
			return Result{}, err
		}
		return Result{}, ErrConflict
	}
	if vm.Status == "stopped" {
		return Result{VM: &vm, AlreadySatisfied: true}, nil
	}
	return c.mutateAndVerify(opCtx, action, http.MethodPost, vmPath(c.node, identity.VMID)+"/status/stop", url.Values{}, identity, "stop")
}

// DestroyProof represents a separately reviewed cleanup decision. Proxmox
// DELETE has no digest/CAS parameter, so an operator must also enforce an
// exclusive reservation and reconcile any concurrent external mutation.
type DestroyProof struct {
	IdleProof
	ApprovalID           string
	ExpectedConfigDigest string
	RunnerDeregistered   bool
	StateReconciled      bool
}

// Destroy refuses templates, running/protected/locked guests, stale evidence,
// config changes, and mismatched marker/pool/storage. It never purges unrelated
// disks or bypasses a Proxmox lock.
func (c *Client) Destroy(ctx context.Context, action Action, identity Identity, proof DestroyProof) (Result, error) {
	if err := validateAction(action); err != nil {
		return Result{}, err
	}
	if err := validateIdleProof(identity, proof.IdleProof, time.Now()); err != nil || !idPattern.MatchString(proof.ApprovalID) || !digestPattern.MatchString(proof.ExpectedConfigDigest) || !proof.RunnerDeregistered || !proof.StateReconciled {
		return Result{}, fmt.Errorf("%w: reviewed cleanup proof required", ErrInvalid)
	}
	opCtx, cancel := context.WithTimeout(ctx, c.operationTimeout)
	defer cancel()
	vm, err := c.Get(opCtx, identity)
	if err != nil {
		return Result{}, err
	}
	if vm.Status != "stopped" || vm.Protected || vm.Locked || vm.ConfigDigest != proof.ExpectedConfigDigest {
		return Result{}, fmt.Errorf("%w: VM not safe for deletion", ErrConflict)
	}
	return c.mutateAndVerify(opCtx, action, http.MethodDelete, vmPath(c.node, identity.VMID)+"?purge=0&destroy-unreferenced-disks=0", nil, identity, "destroy")
}

// Reconcile observes a previously issued operation without issuing it again.
// A lost UPID leaves start/stop/destroy unknown; only a clone can be tied to
// its persisted creation marker without a task ID.
func (c *Client) Reconcile(ctx context.Context, operationID string, identity Identity, kind, upid string) (Result, error) {
	if err := c.validateIdentity(identity); err != nil {
		return Result{}, err
	}
	if !idPattern.MatchString(operationID) || (kind != "clone" && kind != "start" && kind != "stop" && kind != "destroy") {
		return Result{}, ErrInvalid
	}
	opCtx, cancel := context.WithTimeout(ctx, c.operationTimeout)
	defer cancel()
	if upid == "" {
		if kind == "clone" && operationID == identity.CreationOperationID {
			vm, err := c.Get(opCtx, identity)
			if err == nil && !vm.Locked {
				return Result{VM: &vm, AlreadySatisfied: true}, nil
			}
			if err != nil && !errors.Is(err, ErrNotFound) {
				return Result{}, err
			}
		}
		return Result{}, &UnknownOutcomeError{OperationID: operationID, Cause: errors.New("no durable Proxmox task ID")}
	}
	if _, err := parseTaskID(upid, c.node, kind); err != nil {
		return Result{}, err
	}
	if err := c.waitTask(opCtx, upid, identity.VMID, kind); err != nil {
		return Result{}, &UnknownOutcomeError{OperationID: operationID, UPID: upid, Cause: err}
	}
	return c.verifyAfterTask(opCtx, operationID, upid, identity, kind)
}

func (c *Client) mutateAndVerify(ctx context.Context, action Action, method, path string, form url.Values, identity Identity, kind string) (Result, error) {
	var upid string
	status, err := c.request(ctx, method, path, form, &upid)
	if err != nil {
		if status == http.StatusBadRequest || status == http.StatusUnauthorized || status == http.StatusForbidden || status == http.StatusNotFound {
			return Result{}, err
		}
		return Result{}, &UnknownOutcomeError{OperationID: action.ID, Cause: err}
	}
	if _, taskErr := parseTaskID(upid, c.node, kind); taskErr != nil {
		return Result{}, &UnknownOutcomeError{OperationID: action.ID, Cause: fmt.Errorf("%w: %v", ErrProtocol, taskErr)}
	}
	if err := c.waitTask(ctx, upid, identity.VMID, kind); err != nil {
		return Result{}, &UnknownOutcomeError{OperationID: action.ID, UPID: upid, Cause: err}
	}
	return c.verifyAfterTask(ctx, action.ID, upid, identity, kind)
}

func (c *Client) verifyAfterTask(ctx context.Context, operationID, upid string, identity Identity, kind string) (Result, error) {
	if kind == "destroy" {
		resources, err := c.resources(ctx)
		if err != nil {
			return Result{}, &UnknownOutcomeError{OperationID: operationID, UPID: upid, Cause: err}
		}
		if _, exists := resources[identity.VMID]; exists {
			return Result{}, &UnknownOutcomeError{OperationID: operationID, UPID: upid, Cause: ErrConflict}
		}
		return Result{UPID: upid}, nil
	}
	vm, err := c.Get(ctx, identity)
	if err != nil {
		return Result{}, &UnknownOutcomeError{OperationID: operationID, UPID: upid, Cause: err}
	}
	if (kind == "start" && vm.Status != "running") || (kind == "stop" && vm.Status != "stopped") || (kind == "clone" && vm.Status != "stopped") {
		return Result{}, &UnknownOutcomeError{OperationID: operationID, UPID: upid, Cause: ErrConflict}
	}
	return Result{VM: &vm, UPID: upid}, nil
}

func (c *Client) waitTask(ctx context.Context, upid string, vmid int, kind string) error {
	path := "/nodes/" + c.node + "/tasks/" + upid + "/status"
	wantType := taskTypeFor(kind)
	for {
		var task struct {
			UPID       string `json:"upid"`
			Node       string `json:"node"`
			ID         string `json:"id"`
			Type       string `json:"type"`
			Status     string `json:"status"`
			ExitStatus string `json:"exitstatus"`
		}
		if _, err := c.request(ctx, http.MethodGet, path, nil, &task); err != nil {
			return err
		}
		if task.UPID != upid || task.Node != c.node || task.ID != strconv.Itoa(vmid) || task.Type != wantType {
			return ErrProtocol
		}
		if task.Status == "stopped" {
			if task.ExitStatus == "OK" {
				return nil
			}
			return fmt.Errorf("%w: Proxmox task did not finish OK", ErrConflict)
		}
		if task.Status != "running" {
			return ErrProtocol
		}
		timer := time.NewTimer(c.pollInterval)
		select {
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		case <-timer.C:
		}
	}
}
