// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"bytes"
	"fmt"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// These two functions are the only place the control plane decides what a
// finished runner operation actually did. terraformStateHasRunner reads the
// state file Terraform wrote; terraformLifecycleOutcome weighs that reading
// against an independent Proxmox observation and either names an outcome or
// refuses to name one. Everything they let through is recorded as fact, so the
// refusals matter more than the happy paths, and the refusals were what had
// nothing holding them.

func agreedVM() store.RunnerVM {
	return store.RunnerVM{
		ID:                  "2f5a13ed-f770-4f68-a246-52c1e8f7e018",
		CreationOperationID: "4e172436-ec32-4a3d-ad27-c6b0efed58f4",
		RunnerVMInput:       store.RunnerVMInput{VMID: 9020, Name: "ra8-lab-ci-9020"},
	}
}

func runnerMarker(vm store.RunnerVM) string {
	return fmt.Sprintf("RA8CI_RESERVATION=%s;RA8CI_OPERATION=%s;RA8_LAB_RUN=%016x",
		vm.ID, vm.CreationOperationID, uint64(vm.WorkflowRunID))
}

func runnerAttributes(vm store.RunnerVM) map[string]any {
	return map[string]any{"vm_id": vm.VMID, "name": vm.Name, "description": runnerMarker(vm)}
}

func runnerResource(attributes any) map[string]any {
	return map[string]any{
		"type": "proxmox_virtual_environment_vm", "name": "runner",
		"instances": []any{map[string]any{"attributes": attributes}},
	}
}

// stateEnvelope carries a well-formed state identity and whatever module body
// a test hands it, so an identity field and a resource field can be bent one
// at a time.
func stateEnvelope(body map[string]any) []byte {
	envelope := map[string]any{
		"version": 4, "serial": 7,
		"lineage":           "2f5a13ed-f770-4f68-a246-52c1e8f7e018",
		"terraform_version": "1.10.5",
	}
	for key, value := range body {
		envelope[key] = value
	}
	return mustTerraformJSON(envelope)
}

func nestedRunnerState(attributes any) []byte {
	return stateEnvelope(map[string]any{
		"child_modules": []any{map[string]any{"resources": []any{runnerResource(attributes)}}},
	})
}

func stateRefusal(t *testing.T, name string, body []byte, vm store.RunnerVM) error {
	t.Helper()
	found, err := terraformStateHasRunner(body, vm)
	if err == nil {
		t.Fatalf("%s: accepted, found=%t", name, found)
	}
	if found {
		t.Fatalf("%s: refused and still reported a runner present", name)
	}
	return err
}

// TestStateIsJudgedAgainstItsBoundBeforeItIsParsed pins that the size bound is
// a bound on the BYTES, checked before any parse, and that it is exact.
func TestStateIsJudgedAgainstItsBoundBeforeItIsParsed(t *testing.T) {
	vm := agreedVM()
	body := nestedRunnerState(runnerAttributes(vm))

	atBound := append(append([]byte{}, body...), bytes.Repeat([]byte(" "), maxTerraformStateBytes-len(body))...)
	if len(atBound) != maxTerraformStateBytes {
		t.Fatalf("fixture is %d bytes, want exactly the bound", len(atBound))
	}
	found, err := terraformStateHasRunner(atBound, vm)
	if err != nil || !found {
		t.Fatalf("state of exactly the bound rejected: found=%t err=%v", found, err)
	}

	err = stateRefusal(t, "one byte over the bound", append(atBound, ' '), vm)
	if !strings.Contains(err.Error(), "exceeds its bound") {
		t.Fatalf("over-bound refusal reads %q", err)
	}
	if err := stateRefusal(t, "empty state", nil, vm); !strings.Contains(err.Error(), "empty") {
		t.Fatalf("empty refusal reads %q", err)
	}
	if err := stateRefusal(t, "zero-length state", []byte{}, vm); err == nil {
		t.Fatal("zero-length state accepted")
	}
}

// TestUnparseableStateIsRefusedAsMalformed keeps the parse failure separate
// from every later reading, so an operator is told the file is not JSON rather
// than that the runner is missing.
func TestUnparseableStateIsRefusedAsMalformed(t *testing.T) {
	vm := agreedVM()
	for _, body := range []string{"{", "[]", "null", "7", `"state"`, "{\"version\":}"} {
		err := stateRefusal(t, body, []byte(body), vm)
		if !strings.Contains(err.Error(), "malformed") {
			t.Fatalf("%q refused as %q, want malformed", body, err)
		}
	}
}

// TestStateIdentityIsJudgedFieldByField walks the four identity fields. A
// state whose identity does not hold is not evidence about anything, however
// convincing its resources look.
func TestStateIdentityIsJudgedFieldByField(t *testing.T) {
	vm := agreedVM()
	runner := []any{runnerResource(runnerAttributes(vm))}
	tests := []struct {
		name  string
		field string
		value any
	}{
		{name: "state format 3", field: "version", value: 3},
		{name: "state format 5", field: "version", value: 5},
		{name: "state format as text", field: "version", value: "4"},
		{name: "negative serial", field: "serial", value: -1},
		{name: "serial as text", field: "serial", value: "7"},
		{name: "lineage uppercased", field: "lineage", value: "2F5A13ED-F770-4F68-A246-52C1E8F7E018"},
		{name: "lineage truncated", field: "lineage", value: "2f5a13ed-f770-4f68-a246-52c1e8f7e01"},
		{name: "terraform version two parts", field: "terraform_version", value: "1.10"},
		{name: "terraform version tagged", field: "terraform_version", value: "v1.10.5"},
		{name: "terraform version prereleased", field: "terraform_version", value: "1.10.5-rc1"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			body := stateEnvelope(map[string]any{test.field: test.value, "resources": runner})
			err := stateRefusal(t, test.name, body, vm)
			if !strings.Contains(err.Error(), "identity is malformed") {
				t.Fatalf("refused as %q, want the identity refusal", err)
			}
		})
	}

	// A serial of zero is the first write Terraform makes and is admitted.
	body := stateEnvelope(map[string]any{"serial": 0, "resources": runner})
	if found, err := terraformStateHasRunner(body, vm); err != nil || !found {
		t.Fatalf("serial zero rejected: found=%t err=%v", found, err)
	}

	// OpenTofu lineages are go-uuid random hex with no RFC 4122 version or
	// variant bits; the live run's state carried one like these.
	for _, lineage := range []string{
		"2f5a13ed-f770-0f68-a246-52c1e8f7e018",
		"2f5a13ed-f770-4f68-c246-52c1e8f7e018",
		"00000000-0000-0000-0000-000000000000",
	} {
		body := stateEnvelope(map[string]any{"lineage": lineage, "resources": runner})
		if found, err := terraformStateHasRunner(body, vm); err != nil || !found {
			t.Fatalf("lineage %s rejected: found=%t err=%v", lineage, found, err)
		}
	}

	// A pull that returns the encrypted envelope is named as such.
	encrypted := []byte(`{"serial":1,"lineage":"2f5a13ed-f770-4f68-a246-52c1e8f7e018","meta":{},"encrypted_data":"AA==","encryption_version":"v0"}`)
	if _, err := terraformStateHasRunner(encrypted, vm); err == nil || !strings.Contains(err.Error(), "still encrypted") {
		t.Fatalf("encrypted state refused as %v, want the encrypted refusal", err)
	}
}

// TestMissingIdentityFieldsReadAsAMalformedIdentity pins that an absent field
// is refused by the same reading as a wrong one, rather than defaulting.
func TestMissingIdentityFieldsReadAsAMalformedIdentity(t *testing.T) {
	vm := agreedVM()
	full := map[string]any{
		"version": 4, "serial": 7,
		"lineage":           "2f5a13ed-f770-4f68-a246-52c1e8f7e018",
		"terraform_version": "1.10.5",
	}
	for field := range full {
		t.Run(field, func(t *testing.T) {
			partial := map[string]any{}
			for key, value := range full {
				if key != field {
					partial[key] = value
				}
			}
			err := stateRefusal(t, field, mustTerraformJSON(partial), vm)
			if !strings.Contains(err.Error(), "identity is malformed") {
				t.Fatalf("missing %s refused as %q", field, err)
			}
		})
	}
	if err := stateRefusal(t, "empty object", []byte("{}"), vm); err == nil {
		t.Fatal("an empty state object was accepted")
	}
}

// TestIdentityIsJudgedBeforeAnyResource pins the ORDER. A state carrying both
// a broken identity and a duplicate runner reads as the broken identity, which
// is the fact that decides whether anything else in the file means anything.
func TestIdentityIsJudgedBeforeAnyResource(t *testing.T) {
	vm := agreedVM()
	duplicated := []any{runnerResource(runnerAttributes(vm)), runnerResource(runnerAttributes(vm))}
	body := stateEnvelope(map[string]any{"version": 3, "resources": duplicated})
	err := stateRefusal(t, "broken identity over duplicate runners", body, vm)
	if !strings.Contains(err.Error(), "identity is malformed") {
		t.Fatalf("refused as %q, want the identity refusal ahead of the duplicate", err)
	}
}

// TestARunnerIsFoundAtTheRootAndAtDepth pins that the walk is a walk: the
// resource counts wherever the module tree carries it.
func TestARunnerIsFoundAtTheRootAndAtDepth(t *testing.T) {
	vm := agreedVM()
	resource := runnerResource(runnerAttributes(vm))
	root := stateEnvelope(map[string]any{"resources": []any{resource}})
	deep := stateEnvelope(map[string]any{"child_modules": []any{
		map[string]any{"child_modules": []any{
			map[string]any{"child_modules": []any{
				map[string]any{"resources": []any{resource}},
			}},
		}},
	}})
	for name, body := range map[string][]byte{"root module": root, "three modules down": deep} {
		found, err := terraformStateHasRunner(body, vm)
		if err != nil || !found {
			t.Fatalf("%s: found=%t err=%v", name, found, err)
		}
	}
}

// TestAStateWithoutOurRunnerIsAnAnswerNotARefusal is the reading Destroy turns
// on: no runner in the file is a FALSE with no error, and unrelated resources
// never make it one.
func TestAStateWithoutOurRunnerIsAnAnswerNotARefusal(t *testing.T) {
	vm := agreedVM()
	others := []any{
		map[string]any{"type": "proxmox_virtual_environment_vm", "name": "template",
			"instances": []any{map[string]any{"attributes": map[string]any{"vm_id": 9001}}}},
		map[string]any{"type": "proxmox_virtual_environment_pool", "name": "runner",
			"instances": []any{map[string]any{"attributes": map[string]any{"pool_id": "ra8-tf-lab"}}}},
		map[string]any{"type": "null_resource", "name": "runner"},
	}
	bodies := map[string][]byte{
		"no resources at all":    stateEnvelope(nil),
		"an empty resource list": stateEnvelope(map[string]any{"resources": []any{}}),
		"only other resources":   stateEnvelope(map[string]any{"resources": others}),
		"other resources at depth": stateEnvelope(map[string]any{
			"child_modules": []any{map[string]any{"resources": others}}}),
	}
	for name, body := range bodies {
		found, err := terraformStateHasRunner(body, vm)
		if err != nil || found {
			t.Fatalf("%s: found=%t err=%v, want false with no error", name, found, err)
		}
	}
}

// TestTheRunnerIsMatchedOnTypeAndNameTogether pins that both halves of the
// address are required: the type alone under another name, or our name on
// another type, is somebody else's resource.
func TestTheRunnerIsMatchedOnTypeAndNameTogether(t *testing.T) {
	vm := agreedVM()
	// Attributes that would be REFUSED if the resource were ours, so a false
	// answer here proves the resource was skipped rather than read and passed.
	foreign := map[string]any{"vm_id": 4242, "name": "someone-else", "description": "not ours"}
	for name, resource := range map[string]map[string]any{
		"our type, another name": {"type": "proxmox_virtual_environment_vm", "name": "runner_b",
			"instances": []any{map[string]any{"attributes": foreign}}},
		"our name, another type": {"type": "proxmox_virtual_environment_container", "name": "runner",
			"instances": []any{map[string]any{"attributes": foreign}}},
		"name cased differently": {"type": "proxmox_virtual_environment_vm", "name": "Runner",
			"instances": []any{map[string]any{"attributes": foreign}}},
	} {
		body := stateEnvelope(map[string]any{"resources": []any{resource}})
		found, err := terraformStateHasRunner(body, vm)
		if err != nil || found {
			t.Fatalf("%s: found=%t err=%v, want the resource skipped", name, found, err)
		}
	}
}

// TestTheRunnerInstanceCountMustBeExactlyOne pins that a runner resource with
// no instance, or with two, is refused rather than counted or ignored. Either
// shape means the module was edited into something review never saw.
func TestTheRunnerInstanceCountMustBeExactlyOne(t *testing.T) {
	vm := agreedVM()
	attributes := runnerAttributes(vm)
	for name, instances := range map[string]any{
		"no instances":        []any{},
		"two instances":       []any{map[string]any{"attributes": attributes}, map[string]any{"attributes": attributes}},
		"instances as object": map[string]any{"attributes": attributes},
	} {
		resource := map[string]any{
			"type": "proxmox_virtual_environment_vm", "name": "runner", "instances": instances,
		}
		body := stateEnvelope(map[string]any{"resources": []any{resource}})
		err := stateRefusal(t, name, body, vm)
		if !strings.Contains(err.Error(), "instance") {
			t.Fatalf("%s refused as %q", name, err)
		}
	}
}

// TestRunnerIdentityMustMatchTheReservationExactly is the heart of it: the
// state may only vouch for the reservation it was written for.
func TestRunnerIdentityMustMatchTheReservationExactly(t *testing.T) {
	vm := agreedVM()
	other := store.RunnerVM{
		ID:                  "9d92b101-4a78-4f84-9a1f-49095ddb73d7",
		CreationOperationID: "1a8e7d16-93b6-4a9d-9f44-2f9a1b0c5d3e",
		RunnerVMInput:       store.RunnerVMInput{VMID: 9042, Name: "ra8-lab-ci-9042"},
	}
	tests := map[string]map[string]any{
		"another vmid":             {"vm_id": other.VMID, "name": vm.Name, "description": runnerMarker(vm)},
		"another name":             {"vm_id": vm.VMID, "name": other.Name, "description": runnerMarker(vm)},
		"another reservation":      {"vm_id": vm.VMID, "name": vm.Name, "description": runnerMarker(other)},
		"marker halves swapped":    {"vm_id": vm.VMID, "name": vm.Name, "description": "RA8CI_OPERATION=" + vm.CreationOperationID + ";RA8CI_RESERVATION=" + vm.ID},
		"marker with a suffix":     {"vm_id": vm.VMID, "name": vm.Name, "description": runnerMarker(vm) + ";RA8CI_EXTRA=1"},
		"marker with a prefix":     {"vm_id": vm.VMID, "name": vm.Name, "description": " " + runnerMarker(vm)},
		"marker absent":            {"vm_id": vm.VMID, "name": vm.Name},
		"name absent":              {"vm_id": vm.VMID, "description": runnerMarker(vm)},
		"vmid absent":              {"name": vm.Name, "description": runnerMarker(vm)},
		"vmid as text":             {"vm_id": "9020", "name": vm.Name, "description": runnerMarker(vm)},
		"attributes absent":        nil,
		"attributes not an object": {},
	}
	for name, attributes := range tests {
		t.Run(name, func(t *testing.T) {
			var body []byte
			if name == "attributes not an object" {
				body = nestedRunnerState([]any{"attributes"})
			} else {
				body = nestedRunnerState(attributes)
			}
			stateRefusal(t, name, body, vm)
		})
	}
}

// TestTheLegacyVMIDAttributeIsStillRead pins the fallback: a state written by
// the older provider spelling is read rather than refused, and the fallback is
// only consulted when vm_id is absent.
func TestTheLegacyVMIDAttributeIsStillRead(t *testing.T) {
	vm := agreedVM()
	legacy := map[string]any{"vmid": vm.VMID, "name": vm.Name, "description": runnerMarker(vm)}
	if found, err := terraformStateHasRunner(nestedRunnerState(legacy), vm); err != nil || !found {
		t.Fatalf("legacy vmid attribute rejected: found=%t err=%v", found, err)
	}
	// With both present, vm_id decides, so a correct legacy field cannot cover
	// for a current one naming another guest.
	both := map[string]any{"vm_id": 9042, "vmid": vm.VMID, "name": vm.Name, "description": runnerMarker(vm)}
	stateRefusal(t, "vm_id names another guest", nestedRunnerState(both), vm)
}

// TestDuplicateRunnersAreRefusedRatherThanCounted pins that two runners in one
// state file is its own refusal, wherever the second one sits.
func TestDuplicateRunnersAreRefusedRatherThanCounted(t *testing.T) {
	vm := agreedVM()
	resource := runnerResource(runnerAttributes(vm))
	sameModule := stateEnvelope(map[string]any{"resources": []any{resource, resource}})
	acrossModules := stateEnvelope(map[string]any{
		"resources":     []any{resource},
		"child_modules": []any{map[string]any{"resources": []any{resource}}},
	})
	for name, body := range map[string][]byte{"in one module": sameModule, "across modules": acrossModules} {
		err := stateRefusal(t, name, body, vm)
		if !strings.Contains(err.Error(), "duplicate") {
			t.Fatalf("%s refused as %q, want the duplicate refusal", name, err)
		}
	}
}

// TestAMalformedModuleTreeIsRefusedNotWalkedPast pins that a resource list or
// child-module list we cannot read is a refusal, never a quiet zero.
func TestAMalformedModuleTreeIsRefusedNotWalkedPast(t *testing.T) {
	vm := agreedVM()
	runner := runnerResource(runnerAttributes(vm))
	for name, body := range map[string][]byte{
		"resources as an object":     stateEnvelope(map[string]any{"resources": map[string]any{"type": "x"}}),
		"a resource as text":         stateEnvelope(map[string]any{"resources": []any{"runner"}}),
		"child modules as an object": stateEnvelope(map[string]any{"child_modules": map[string]any{"resources": []any{}}}),
		"a child module as text":     stateEnvelope(map[string]any{"child_modules": []any{"module"}}),
		"a broken module beside a good runner": stateEnvelope(map[string]any{
			"resources":     []any{runner},
			"child_modules": []any{map[string]any{"resources": map[string]any{}}},
		}),
	} {
		err := stateRefusal(t, name, body, vm)
		if !strings.Contains(err.Error(), "malformed") {
			t.Fatalf("%s refused as %q", name, err)
		}
	}
}

// TestOneObservationMeansFourDifferentThings is the whole point of
// terraformLifecycleOutcome: the same pair of facts is a success for one
// operation and a failure for another, and the kind is what decides.
func TestOneObservationMeansFourDifferentThings(t *testing.T) {
	want := map[string]string{"clone": "succeeded", "start": "failed", "stop": "succeeded", "destroy": "failed"}
	for kind, expected := range want {
		got, err := terraformLifecycleOutcome(kind, true, false, "stopped")
		if err != nil || got != expected {
			t.Fatalf("%s of a present, stopped guest = %q, %v; want %q", kind, got, err, expected)
		}
	}
}

// TestEveryLifecycleReadingAndEveryRefusal walks the full matrix. A reading
// that is not listed here is an inconsistency, and an inconsistency is refused
// rather than guessed at, because the alternative is recording an outcome no
// operator can trust.
func TestEveryLifecycleReadingAndEveryRefusal(t *testing.T) {
	tests := []struct {
		kind     string
		hasState bool
		absent   bool
		status   string
		want     string
	}{
		{kind: "clone", hasState: true, status: "stopped", want: "succeeded"},
		{kind: "clone", absent: true, want: "failed"},
		{kind: "clone", hasState: true, status: "running"},
		{kind: "clone", hasState: true, status: ""},
		{kind: "clone", hasState: true, absent: true, status: "stopped"},
		{kind: "clone", status: "stopped"},

		{kind: "start", hasState: true, status: "running", want: "succeeded"},
		{kind: "start", hasState: true, status: "stopped", want: "failed"},
		{kind: "start", absent: true},
		{kind: "start", hasState: true, status: "paused"},
		{kind: "start", status: "running"},

		{kind: "stop", hasState: true, status: "stopped", want: "succeeded"},
		{kind: "stop", hasState: true, status: "running", want: "failed"},
		{kind: "stop", absent: true},
		{kind: "stop", hasState: true, status: "suspended"},
		{kind: "stop", status: "stopped"},

		{kind: "destroy", absent: true, want: "succeeded"},
		{kind: "destroy", hasState: true, status: "stopped", want: "failed"},
		{kind: "destroy", hasState: true, status: "running"},
		{kind: "destroy", hasState: true, absent: true},
		{kind: "destroy", status: "stopped"},
	}
	for _, test := range tests {
		name := test.kind + "/state=" + boolWord(test.hasState) + "/absent=" + boolWord(test.absent) + "/status=" + test.status
		t.Run(name, func(t *testing.T) {
			got, err := terraformLifecycleOutcome(test.kind, test.hasState, test.absent, test.status)
			if test.want == "" {
				if err == nil {
					t.Fatalf("named %q for facts that do not agree", got)
				}
				if !strings.Contains(err.Error(), "inconsistent") {
					t.Fatalf("refusal reads %q, want the inconsistency reading", err)
				}
				if got != "" {
					t.Fatalf("refused and still returned %q", got)
				}
				return
			}
			if err != nil || got != test.want {
				t.Fatalf("outcome = %q, %v; want %q", got, err, test.want)
			}
		})
	}
}

// TestAnUnknownOperationKindNamesNoOutcome pins that the switch fails closed.
// A kind added to the store without a reading here refuses rather than
// borrowing another kind's answer.
func TestAnUnknownOperationKindNamesNoOutcome(t *testing.T) {
	for _, kind := range []string{"", "reconcile", "Clone", "CLONE", "clone ", "restart", "delete"} {
		if got, err := terraformLifecycleOutcome(kind, true, false, "stopped"); err == nil {
			t.Fatalf("kind %q named outcome %q", kind, got)
		}
	}
}

// TestAnAbsentGuestIsReadWithoutItsStatus pins that the two readings taken
// against an absent guest ignore the status string, which the caller leaves
// empty exactly then.
func TestAnAbsentGuestIsReadWithoutItsStatus(t *testing.T) {
	for _, status := range []string{"", "running", "stopped", "unknown"} {
		if got, err := terraformLifecycleOutcome("clone", false, true, status); err != nil || got != "failed" {
			t.Fatalf("clone of an absent guest with status %q = %q, %v", status, got, err)
		}
		if got, err := terraformLifecycleOutcome("destroy", false, true, status); err != nil || got != "succeeded" {
			t.Fatalf("destroy of an absent guest with status %q = %q, %v", status, got, err)
		}
	}
}

func boolWord(value bool) string {
	if value {
		return "yes"
	}
	return "no"
}
