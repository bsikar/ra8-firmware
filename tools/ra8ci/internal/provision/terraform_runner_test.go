// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"encoding/json"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func terraformTestState(vm store.RunnerVM) []byte {
	return mustTerraformJSON(map[string]any{
		"version": 4, "serial": 7, "lineage": "2f5a13ed-f770-4f68-a246-52c1e8f7e018",
		"terraform_version": "1.10.5",
		"child_modules": []any{map[string]any{
			"resources": []any{map[string]any{
				"type": "proxmox_virtual_environment_vm", "name": "runner",
				"instances": []any{map[string]any{"attributes": map[string]any{
					"vm_id": vm.VMID, "name": vm.Name,
					"description": "RA8CI_RESERVATION=" + vm.ID + ";RA8CI_OPERATION=" + vm.CreationOperationID,
				}}},
			}},
		}},
	})
}

func mustTerraformJSON(value any) []byte {
	body, err := json.Marshal(value)
	if err != nil {
		panic(err)
	}
	return body
}

func TestTerraformStateRequiresExactNestedRunnerMarker(t *testing.T) {
	vm := store.RunnerVM{ID: "2f5a13ed-f770-4f68-a246-52c1e8f7e018",
		CreationOperationID: "4e172436-ec32-4a3d-ad27-c6b0efed58f4",
		RunnerVMInput:       store.RunnerVMInput{VMID: 9000, Name: "ra8-lab-ci-9000"}}
	found, err := terraformStateHasRunner(terraformTestState(vm), vm)
	if err != nil || !found {
		t.Fatalf("exact runner state rejected: found=%t err=%v", found, err)
	}
	bad := vm
	bad.CreationOperationID = "9d92b101-4a78-4f84-9a1f-49095ddb73d7"
	if _, err := terraformStateHasRunner(terraformTestState(vm), bad); err == nil {
		t.Fatal("accepted Terraform state with another operation marker")
	}
}

func TestTerraformLifecycleOutcomeRequiresConsistentIndependentFacts(t *testing.T) {
	tests := []struct {
		name     string
		kind     string
		hasState bool
		absent   bool
		status   string
		want     string
		wantErr  bool
	}{
		{name: "clone success", kind: "clone", hasState: true, status: "stopped", want: "succeeded"},
		{name: "clone no effect", kind: "clone", absent: true, want: "failed"},
		{name: "start success", kind: "start", hasState: true, status: "running", want: "succeeded"},
		{name: "start no effect", kind: "start", hasState: true, status: "stopped", want: "failed"},
		{name: "destroy success", kind: "destroy", absent: true, want: "succeeded"},
		{name: "inconsistent clone", kind: "clone", hasState: true, absent: true, wantErr: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got, err := terraformLifecycleOutcome(test.kind, test.hasState, test.absent, test.status)
			if (err != nil) != test.wantErr || got != test.want {
				t.Fatalf("outcome = %q, %v; want %q, wantErr=%t", got, err, test.want, test.wantErr)
			}
		})
	}
}
func TestRunnerIPv4RequiresCanonicalHostGatewayPair(t *testing.T) {
	tests := []struct {
		address string
		gateway string
		valid   bool
	}{
		{address: "10.250.8.42/24", gateway: "10.250.8.1", valid: true},
		{address: "10.250.9.18/24", gateway: "10.250.9.1", valid: true},
		{address: "10.250.8.42/24", gateway: "10.250.9.1"},
		{address: "10.250.8.1/24", gateway: "10.250.8.1"},
		{address: "10.250.8.255/24", gateway: "10.250.8.1"},
		{address: "10.250.8.999/24", gateway: "10.250.8.1"},
		{address: "fd00::2/64", gateway: "fd00::1"},
	}
	for _, test := range tests {
		if got := validRunnerIPv4(test.address, test.gateway); got != test.valid {
			t.Errorf("validRunnerIPv4(%q, %q)=%t, want %t", test.address, test.gateway, got, test.valid)
		}
	}
}
