// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The provisioning boundary is the last place a disposable guest can be
// refused cheaply. Past the constructor every check costs a Terraform plan, a
// Proxmox round trip, or a guest that boots onto a network review never saw,
// so these tests hold the admission rule from outside: what a reviewed profile
// must look like, and which single field being wrong is enough to refuse the
// whole provisioner.

type policyLedger struct{}

func (policyLedger) GetRunnerVM(context.Context, string) (store.RunnerVM, error) {
	return store.RunnerVM{}, nil
}

func (policyLedger) GetRunnerVMByJob(context.Context, int64, string) (store.RunnerVM, error) {
	return store.RunnerVM{}, store.ErrNotFound
}

func (policyLedger) ReserveRunnerVM(context.Context, string, store.RunnerVMInput, time.Time) (store.RunnerVM, bool, error) {
	return store.RunnerVM{}, false, nil
}

func (policyLedger) ListActiveRunnerVMIDs(context.Context, string) ([]int, error) {
	return nil, nil
}

func (policyLedger) GetRunnerVMOperation(context.Context, string) (store.RunnerVMOperation, error) {
	return store.RunnerVMOperation{}, nil
}

func (policyLedger) RecordRunnerVMTerraformPlan(context.Context, string, string, int64, string,
	store.RunnerVMTerraformPlanEvidence) error {
	return nil
}

func (policyLedger) BeginRunnerVMTerraformApply(context.Context, string, string, int64, string, string) (bool, error) {
	return false, nil
}

func (policyLedger) ReadRunnerVMTerraformState(context.Context, string) ([]byte, bool, error) {
	return nil, false, nil
}

func (policyLedger) RunnerVMTerraformStateLocked(context.Context, string) (bool, error) {
	return false, nil
}

type policyObserver struct{}

func (policyObserver) Get(context.Context, proxmox.Identity) (proxmox.VM, error) {
	return proxmox.VM{}, proxmox.ErrNotFound
}

func (policyObserver) GetTemplate(context.Context, int) (proxmox.Template, error) {
	return proxmox.Template{VMID: 9001, Node: "pve1", Pool: "ra8-tf-lab", Name: "ra8-lab-debian-template",
		Status: "stopped", Template: true, ConfigDigest: strings.Repeat("a", 40)}, nil
}

func (policyObserver) OccupiedVMIDs(context.Context) ([]int, error) { return nil, nil }

// reviewedProfile is the healthy shape every refusal below is one edit away
// from: VMID 9020 cloned from template 9001, on vmbr9 and addressed inside the
// /24 that bridge carries.
func reviewedProfile() TerraformRunnerProfile {
	return TerraformRunnerProfile{
		TemplateVMID: 9001,
		TemplateName: "ra8-lab-debian-template",
		Node:         "pve1",
		Pool:         "ra8-tf-lab",
		DatastoreID:  "ra8-tf-lab",
		Bridge:       "vmbr9",
		Cores:        4,
		MemoryMB:     8192,
		IPv4Address:  "10.250.9.20/24",
		IPv4Gateway:  "10.250.9.1",
		UserName:     "ra8ci",
	}
}

func reviewedConfig(t *testing.T) TerraformRunnerConfig {
	t.Helper()
	return TerraformRunnerConfig{
		Actor:             "ra8ci-controller",
		ProxmoxEndpoint:   "https://pve1.example.test:8006",
		OpenBaoAddress:    "https://bao.example.test:8200",
		OpenBaoKVMount:    "kv",
		OpenBaoSecretPath: "ra8ci/proxmox",
		Profiles:          map[int]TerraformRunnerProfile{9020: reviewedProfile()},
		ModuleDirectory:   t.TempDir(),
	}
}

func openProvisioner(t *testing.T, config TerraformRunnerConfig) (*TerraformRunnerProvisioner, error) {
	t.Helper()
	keys, err := NewSSHAccessStore(filepath.Join(t.TempDir(), "keys"))
	if err != nil {
		t.Fatalf("prepare SSH access store: %v", err)
	}
	return NewTerraformRunnerProvisioner(&TerraformRuntime{}, policyLedger{}, policyObserver{}, keys, config)
}

// withProfile edits one field of the reviewed profile and returns the whole
// config, so every refusal below differs from the accepted one in exactly one
// place.
func withProfile(t *testing.T, edit func(*TerraformRunnerProfile)) TerraformRunnerConfig {
	t.Helper()
	config := reviewedConfig(t)
	profile := reviewedProfile()
	edit(&profile)
	config.Profiles = map[int]TerraformRunnerProfile{9020: profile}
	return config
}

func TestTheReviewedProfileIsAdmitted(t *testing.T) {
	provisioner, err := openProvisioner(t, reviewedConfig(t))
	if err != nil || provisioner == nil {
		t.Fatalf("reviewed disposable policy refused: %v", err)
	}
}

func TestLifecycleVMIDRangeRefusesAdjacentIDs(t *testing.T) {
	for _, vmid := range []int{9019, 9040, 9000, 9099} {
		if validLifecycleVMID(vmid) {
			t.Errorf("VMID %d is outside the lifecycle sub-range and must be refused", vmid)
		}
	}
	for _, vmid := range []int{9020, 9039} {
		if !validLifecycleVMID(vmid) {
			t.Errorf("VMID %d is inside the lifecycle sub-range and must be accepted", vmid)
		}
	}
}

// The profiles map is copied on admission. An operator holding the map they
// passed in must not be able to widen the policy afterwards, because every
// later check reads the provisioner's copy.
func TestTheAdmittedPolicyIsACopy(t *testing.T) {
	config := reviewedConfig(t)
	provisioner, err := openProvisioner(t, config)
	if err != nil {
		t.Fatalf("reviewed disposable policy refused: %v", err)
	}
	config.Profiles[9021] = reviewedProfile()
	widened := reviewedProfile()
	widened.Bridge = "vmbr0"
	config.Profiles[9020] = widened
	if len(provisioner.config.Profiles) != 19 {
		t.Fatalf("admitted policy contains %d profiles, want 19 Linux VMIDs with the Windows slot excluded", len(provisioner.config.Profiles))
	}
	if _, exists := provisioner.config.Profiles[9021]; exists {
		t.Fatal("Linux runner profile must not claim the fixed Windows VMID 9021")
	}
	if provisioner.config.Profiles[9020].Bridge != "vmbr9" {
		t.Fatalf("admitted profile now names bridge %q", provisioner.config.Profiles[9020].Bridge)
	}
}

// Each of these is a single field away from the accepted profile. The VMID
// window, the pool and the datastore are the three that keep a disposable
// guest inside the reviewed lab rather than anywhere else on the host.
func TestOneFieldOutsideThePolicyRefusesTheProvisioner(t *testing.T) {
	tests := []struct {
		name string
		edit func(*TerraformRunnerProfile)
	}{
		{name: "template below the window", edit: func(p *TerraformRunnerProfile) { p.TemplateVMID = 8999 }},
		{name: "template above the window", edit: func(p *TerraformRunnerProfile) { p.TemplateVMID = 9100 }},
		{name: "wrong template name", edit: func(p *TerraformRunnerProfile) { p.TemplateName = "other-template" }},
		{name: "another node", edit: func(p *TerraformRunnerProfile) { p.Node = "pve2" }},
		{name: "unnamed node", edit: func(p *TerraformRunnerProfile) { p.Node = "" }},
		{name: "node with a space", edit: func(p *TerraformRunnerProfile) { p.Node = "pve lab 1" }},
		{name: "another pool", edit: func(p *TerraformRunnerProfile) { p.Pool = "default" }},
		{name: "no pool", edit: func(p *TerraformRunnerProfile) { p.Pool = "" }},
		{name: "another datastore", edit: func(p *TerraformRunnerProfile) { p.DatastoreID = "local-lvm" }},
		{name: "no datastore", edit: func(p *TerraformRunnerProfile) { p.DatastoreID = "" }},
		{name: "the management bridge", edit: func(p *TerraformRunnerProfile) { p.Bridge = "vmbr0" }},
		{name: "an unreviewed bridge", edit: func(p *TerraformRunnerProfile) { p.Bridge = "vmbr7" }},
		{name: "no cores", edit: func(p *TerraformRunnerProfile) { p.Cores = 0 }},
		{name: "too few cores", edit: func(p *TerraformRunnerProfile) { p.Cores = 3 }},
		{name: "too many cores", edit: func(p *TerraformRunnerProfile) { p.Cores = 5 }},
		{name: "memory under the floor", edit: func(p *TerraformRunnerProfile) { p.MemoryMB = 511 }},
		{name: "memory below ceiling", edit: func(p *TerraformRunnerProfile) { p.MemoryMB = 4096 }},
		{name: "memory over the ceiling", edit: func(p *TerraformRunnerProfile) { p.MemoryMB = 8193 }},
		{name: "another user", edit: func(p *TerraformRunnerProfile) { p.UserName = "root" }},
		{name: "no user", edit: func(p *TerraformRunnerProfile) { p.UserName = "" }},
		{name: "no address", edit: func(p *TerraformRunnerProfile) { p.IPv4Address = "" }},
		{name: "address off the lab network", edit: func(p *TerraformRunnerProfile) {
			p.IPv4Address = "192.168.1.42/24"
			p.IPv4Gateway = "192.168.1.1"
		}},
		{name: "gateway from another segment", edit: func(p *TerraformRunnerProfile) { p.IPv4Gateway = "10.250.8.1" }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if _, err := openProvisioner(t, withProfile(t, test.edit)); err == nil {
				t.Fatal("a profile outside the reviewed disposable policy was admitted")
			}
		})
	}
}

// The reviewed runner profile fixes the exact resource ceiling and subnet.
func TestTheProfileUsesTheApprovedResourceCeiling(t *testing.T) {
	profile := reviewedProfile()
	if profile.TemplateVMID != 9001 || profile.TemplateName != "ra8-lab-debian-template" ||
		profile.Bridge != "vmbr9" || profile.Cores != 4 || profile.MemoryMB != 8192 ||
		profile.Pool != "ra8-tf-lab" || profile.DatastoreID != "ra8-tf-lab" {
		t.Fatalf("reviewed profile does not encode the lifecycle boundary: %+v", profile)
	}
}

// The VMID a profile is keyed by is held to the same window as the template it
// clones, and the two may never be the same guest: a clone whose target is its
// own template destroys the template.
func TestTheDisposableVMIDIsHeldToItsOwnWindow(t *testing.T) {
	for _, vmid := range []int{0, 8999, 9100, 10000} {
		config := reviewedConfig(t)
		config.Profiles = map[int]TerraformRunnerProfile{vmid: reviewedProfile()}
		if _, err := openProvisioner(t, config); err == nil {
			t.Fatalf("VMID %d outside the disposable window was admitted", vmid)
		}
	}
	config := reviewedConfig(t)
	self := reviewedProfile()
	self.TemplateVMID = 9020
	config.Profiles = map[int]TerraformRunnerProfile{9020: self}
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a profile cloning itself was admitted")
	}
}

// A VMID configured as a disposable runner in one profile and as the clone
// template of another would be destroyed as disposable and cloned from as a
// template, so the two roles stay disjoint across the whole map.
func TestARunnerVMIDIsNeverAlsoATemplate(t *testing.T) {
	config := reviewedConfig(t)
	first := reviewedProfile()
	first.TemplateVMID = 9001
	second := reviewedProfile()
	second.TemplateVMID = 9050
	second.IPv4Address = "10.250.9.43/24"
	config.Profiles = map[int]TerraformRunnerProfile{9020: first, 9001: second}
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a runner VMID configured as another profile's template was admitted")
	}
}

// Two disposable guests sharing a static address is a lab that half works:
// both provision, and whichever boots second is unreachable or steals the
// other's traffic. The refusal names both VMIDs so an operator can find them.
func TestTwoProfilesMayNotShareAStaticAddress(t *testing.T) {
	config := reviewedConfig(t)
	config.Profiles = map[int]TerraformRunnerProfile{9020: reviewedProfile(), 9022: reviewedProfile()}
	_, err := openProvisioner(t, config)
	if err == nil {
		t.Fatal("two profiles sharing a static address were admitted")
	}
	if !strings.Contains(err.Error(), "9020") || !strings.Contains(err.Error(), "9022") {
		t.Fatalf("the shared-address refusal names neither VMID: %v", err)
	}
}

// The bridge and the address are checked against each other, not only
// separately. Both halves here are individually valid lab settings; the pair
// is a guest on vmbr8 addressed for vmbr9, which fails in the lab as a runner
// that never answers rather than at this boundary.
func TestAProfileIsAddressedForTheBridgeItNames(t *testing.T) {
	crossed := withProfile(t, func(p *TerraformRunnerProfile) {
		p.Bridge = "vmbr8"
		p.IPv4Address = "10.250.9.42/24"
		p.IPv4Gateway = "10.250.9.1"
	})
	if !validRunnerIPv4("10.250.9.42/24", "10.250.9.1") {
		t.Fatal("the crossed address is not a lab address at all, so the test proves nothing")
	}
	if _, err := openProvisioner(t, crossed); err == nil {
		t.Fatal("a profile addressed for another bridge's segment was admitted")
	}
	matched := withProfile(t, func(p *TerraformRunnerProfile) {
		p.Bridge = "vmbr9"
		p.IPv4Address = "10.250.9.42/24"
		p.IPv4Gateway = "10.250.9.1"
	})
	if _, err := openProvisioner(t, matched); err != nil {
		t.Fatalf("a profile on its own bridge's segment was refused: %v", err)
	}
}

// Every reviewed bridge carries the segment its name states, and nothing else
// does. This is what keeps the pairing out of a second table that could
// disagree with the reviewed set.
func TestABridgeCarriesTheSegmentItsNameStates(t *testing.T) {
	if segment, reviewed := runnerBridgeSegment("vmbr8"); !reviewed || segment != 8 {
		t.Fatalf("vmbr8 carries segment %d (reviewed=%t)", segment, reviewed)
	}
	if segment, reviewed := runnerBridgeSegment("vmbr9"); !reviewed || segment != 9 {
		t.Fatalf("vmbr9 carries segment %d (reviewed=%t)", segment, reviewed)
	}
	for _, name := range []string{"", "vmbr0", "vmbr7", "vmbr10", "VMBR8", "br8"} {
		if segment, reviewed := runnerBridgeSegment(name); reviewed || segment != 0 {
			t.Fatalf("unreviewed bridge %q carries segment %d", name, segment)
		}
	}
}

// The provisioner is wired to a runtime, a ledger, an observer and a key
// store, and a nil in any of them is a partly built provisioner that would
// fail at the first lifecycle call instead of at construction.
func TestEveryCollaboratorIsRequired(t *testing.T) {
	keys, err := NewSSHAccessStore(filepath.Join(t.TempDir(), "keys"))
	if err != nil {
		t.Fatalf("prepare SSH access store: %v", err)
	}
	config := reviewedConfig(t)
	if _, err := NewTerraformRunnerProvisioner(nil, policyLedger{}, policyObserver{}, keys, config); err == nil {
		t.Fatal("a provisioner with no Terraform runtime was admitted")
	}
	if _, err := NewTerraformRunnerProvisioner(&TerraformRuntime{}, nil, policyObserver{}, keys, config); err == nil {
		t.Fatal("a provisioner with no ledger was admitted")
	}
	if _, err := NewTerraformRunnerProvisioner(&TerraformRuntime{}, policyLedger{}, nil, keys, config); err == nil {
		t.Fatal("a provisioner with no observer was admitted")
	}
	if _, err := NewTerraformRunnerProvisioner(&TerraformRuntime{}, policyLedger{}, policyObserver{}, nil, config); err == nil {
		t.Fatal("a provisioner with no SSH access store was admitted")
	}
}

// An empty profile map is the dangerous default: nothing is refused for being
// outside the policy because the policy admits nothing at all, and the
// provisioner would sit there looking configured.
func TestAPolicyWithNoProfilesIsRefused(t *testing.T) {
	config := reviewedConfig(t)
	config.Profiles = nil
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a provisioner with no profiles was admitted")
	}
	config.Profiles = map[int]TerraformRunnerProfile{}
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a provisioner with an empty profile map was admitted")
	}
}

// The module directory is the fixed Terraform the provisioner runs. A missing
// directory, a file, or a symlink pointing at one are each refused, because a
// symlink is the way the reviewed module is swapped without the path changing.
func TestTheFixedModuleDirectoryIsCheckedNotAssumed(t *testing.T) {
	config := reviewedConfig(t)
	config.ModuleDirectory = "infra/terraform/runner"
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a relative module directory was admitted")
	}
	config = reviewedConfig(t)
	config.ModuleDirectory = filepath.Join(t.TempDir(), "absent")
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a module directory that does not exist was admitted")
	}
	root := t.TempDir()
	file := filepath.Join(root, "main.tf")
	if err := os.WriteFile(file, []byte("# fixed module\n"), 0o600); err != nil {
		t.Fatalf("write module file: %v", err)
	}
	config = reviewedConfig(t)
	config.ModuleDirectory = file
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a module path that is a file was admitted")
	}
	link := filepath.Join(root, "linked")
	symlinkTest(t, t.TempDir(), link)
	config = reviewedConfig(t)
	config.ModuleDirectory = link
	if _, err := openProvisioner(t, config); err == nil {
		t.Fatal("a symlinked module directory was admitted")
	}
}

// The endpoint every Terraform operation is aimed at. It is an HTTPS origin
// and nothing more: no credentials in the URL, no path or query to redirect
// the call, and no personal-network host, so the lab is reached the way review
// saw it rather than through somebody's tailnet.
func TestTheProxmoxOriginIsAReviewedHTTPSOrigin(t *testing.T) {
	endpoint, err := validateTerraformOrigin("https://pve-lab-1.example.test:8006")
	if err != nil || endpoint != "https://pve-lab-1.example.test:8006" {
		t.Fatalf("reviewed origin = %q, %v", endpoint, err)
	}
	for _, raw := range []string{
		"",
		" https://pve-lab-1.example.test:8006",
		"https://pve-lab-1.example.test:8006 ",
		"http://pve-lab-1.example.test:8006",
		"https://pve-lab-1.example.test",
		"https://:8006",
		"https://user:secret@pve-lab-1.example.test:8006",
		"https://pve-lab-1.example.test:8006/api2/json",
		"https://pve-lab-1.example.test:8006/",
		"https://pve-lab-1.example.test:8006?node=pve",
		"https://pve-lab-1.example.test:8006#fragment",
		"https://pve-lab-1.tailnet.ts.net:8006",
		"https://100.64.0.1:8006",
	} {
		if _, err := validateTerraformOrigin(raw); err == nil {
			t.Fatalf("unreviewed Proxmox origin %q accepted", raw)
		}
	}
}

// The origin rule is enforced at construction, not only by the helper, so a
// provisioner can never be built pointing somewhere review did not see.
func TestTheProvisionerRefusesAnUnreviewedOrigin(t *testing.T) {
	for _, raw := range []string{"", "http://pve-lab-1.example.test:8006",
		"https://pve-lab-1.example.test:8006/api2/json", "https://100.64.0.1:8006"} {
		config := reviewedConfig(t)
		config.ProxmoxEndpoint = raw
		if _, err := openProvisioner(t, config); err == nil {
			t.Fatalf("a provisioner aimed at %q was admitted", raw)
		}
	}
}

// The secret wiring the runtime needs to reach OpenBao at all. Each is
// required on its own, so a half-configured secret path is refused here rather
// than discovered as an unauthenticated Terraform run.
func TestTheSecretWiringIsRequiredInFull(t *testing.T) {
	tests := []struct {
		name string
		edit func(*TerraformRunnerConfig)
	}{
		{name: "no actor", edit: func(c *TerraformRunnerConfig) { c.Actor = "" }},
		{name: "oversized actor", edit: func(c *TerraformRunnerConfig) { c.Actor = strings.Repeat("a", 257) }},
		{name: "no OpenBao address", edit: func(c *TerraformRunnerConfig) { c.OpenBaoAddress = "" }},
		{name: "blank OpenBao address", edit: func(c *TerraformRunnerConfig) { c.OpenBaoAddress = "   " }},
		{name: "no KV mount", edit: func(c *TerraformRunnerConfig) { c.OpenBaoKVMount = "" }},
		{name: "no secret path", edit: func(c *TerraformRunnerConfig) { c.OpenBaoSecretPath = "" }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			config := reviewedConfig(t)
			test.edit(&config)
			if _, err := openProvisioner(t, config); err == nil {
				t.Fatal("an incompletely wired provisioner was admitted")
			}
		})
	}
}
