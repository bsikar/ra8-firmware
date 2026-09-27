// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
)

const (
	shippedGateScript   = "scripts/ci.sh"
	shippedFormatTree   = "scripts/checks/format_tree.sh"
	unshippedGateScript = "scripts/checks/does_not_exist.sh"
)

func shipStep(args ...string) Step {
	return Step{Name: "gate", Program: DispatchShell, Args: args}
}

func TestAReviewedScriptIsAdmitted(t *testing.T) {
	for _, script := range ReviewedScriptPaths() {
		if err := checkTheScriptIsOneTheCheckoutShips(shipStep(script)); err != nil {
			t.Fatalf("%s is a reviewed script and must be admitted: %v", script, err)
		}
	}
}

func TestAScriptNothingShipsIsRefused(t *testing.T) {
	err := checkTheScriptIsOneTheCheckoutShips(shipStep(unshippedGateScript))
	if err == nil {
		t.Fatal("a script no reviewed list names must be refused")
	}
	for _, want := range []string{unshippedGateScript, "not a reviewed script", "exit 127", shippedGateScript} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q: %v", want, err)
		}
	}
}

// The mistake that motivated the door: a path whose SHAPE is perfect, so every
// rule that stood before this one admitted it.
func TestAWellShapedPathIsNotEnough(t *testing.T) {
	step := shipStep(unshippedGateScript)
	if !ValidScriptPath(step.Args[0]) {
		t.Fatal("the fixture must be a well-shaped path, or it proves nothing")
	}
	if err := checkTheScriptIsOneTheCheckoutShips(step); err == nil {
		t.Fatal("a well-shaped path to a script nothing ships must still be refused")
	}
}

// A script outside scripts/ entirely. Refused for the same reason and by the
// same rule: the list is the dispatch surface, not the directory.
func TestAScriptOutsideTheReviewedListIsRefused(t *testing.T) {
	for _, path := range []string{"tools/ci/gate.sh", "scripts/ci/lib/abort.sh", "x.sh"} {
		if err := checkTheScriptIsOneTheCheckoutShips(shipStep(path)); err == nil {
			t.Fatalf("%q is not a reviewed dispatch target and must be refused", path)
		}
	}
}

// Comparison is EXACT, the way the executor's argv reaches bash. A spelling
// that resolves to the same file on a POSIX filesystem is still a different
// string in a reviewed definition, and review pins the string.
func TestTheComparisonIsExact(t *testing.T) {
	for _, path := range []string{"./scripts/ci.sh", "scripts//ci.sh", "scripts/ci.SH", "scripts/ci.sh "} {
		if err := checkTheScriptIsOneTheCheckoutShips(shipStep(path)); err == nil {
			t.Fatalf("%q must not pass as %q", path, shippedGateScript)
		}
	}
}

// Wiring: the door stands inside ValidateStepDispatch, not only as a function.
func TestTheDoorStandsOnTheShellBranch(t *testing.T) {
	if err := ValidateStepDispatch(shipStep(unshippedGateScript)); err == nil {
		t.Fatal("ValidateStepDispatch must refuse a step dispatching an unreviewed script")
	}
	if err := ValidateStepDispatch(shipStep(shippedFormatTree)); err != nil {
		t.Fatalf("a reviewed script with no arguments must still pass: %v", err)
	}
}

// Order: a script nothing ships is refused for WHAT IT NAMES, never for what
// it hands that script. Refusing the argv first would name the wrong mistake,
// and would tell a reviewer to fix an option on a step whose real problem is
// that no such script exists.
func TestTheExistenceRefusalComesBeforeTheOptionDoors(t *testing.T) {
	err := ValidateStepDispatch(shipStep(unshippedGateScript, "--not-a-flag-anything-parses"))
	if err == nil {
		t.Fatal("expected a refusal")
	}
	if !strings.Contains(err.Error(), "not a reviewed script") {
		t.Fatalf("the existence door must speak first: %v", err)
	}
}

// A malformed path stays the path rule's refusal. This door judges a path that
// is already well formed and never takes that rule's message.
func TestAMalformedPathKeepsItsOwnRefusal(t *testing.T) {
	for _, path := range []string{"scripts/../ci.sh", "/etc/ci.sh", "-i.sh", "scripts/ci.txt"} {
		err := ValidateStepDispatch(shipStep(path))
		if err == nil {
			t.Fatalf("%q must be refused", path)
		}
		if strings.Contains(err.Error(), "not a reviewed script") {
			t.Fatalf("%q is malformed and must keep the path rule's refusal: %v", path, err)
		}
	}
}

// A step naming no script at all is the path rule's refusal too, and this door
// must never index past the end of an empty argv reaching for one.
func TestAStepWithNoScriptNeverReachesThisDoor(t *testing.T) {
	err := ValidateStepDispatch(Step{Name: "gate", Program: DispatchShell})
	if err == nil {
		t.Fatal("a shell step naming no script must be refused")
	}
	if strings.Contains(err.Error(), "not a reviewed script") {
		t.Fatalf("an empty argv is the path rule's refusal: %v", err)
	}
}

// A tool step walks past this door untouched; it has its own name rule.
func TestAToolStepIsNotJudgedByTheScriptDoor(t *testing.T) {
	step := Step{Name: "scan", Program: "ra8ci:ascii", Args: []string{"--all"}}
	if err := ValidateStepDispatch(step); err != nil {
		t.Fatalf("a reviewed tool step must be unaffected: %v", err)
	}
}

// Every entry on the list must itself be a path the shape rule accepts, or the
// door would admit a script the rule before it refuses.
func TestEveryReviewedScriptIsAWellShapedPath(t *testing.T) {
	seen := make(map[string]bool, len(dispatchableScripts))
	for _, script := range dispatchableScripts {
		if !ValidScriptPath(script) {
			t.Fatalf("%q is on the reviewed list but is not a well-shaped script path", script)
		}
		if seen[script] {
			t.Fatalf("%q is named twice on the reviewed list", script)
		}
		seen[script] = true
	}
	if len(dispatchableScripts) == 0 {
		t.Fatal("the shell branch must dispatch at least one reviewed script")
	}
}

// ReviewedScriptPaths hands back a copy, so a caller cannot edit the list the
// door reads. Same contract ReviewedToolPrograms keeps.
func TestReviewedScriptPathsIsACopy(t *testing.T) {
	paths := ReviewedScriptPaths()
	if len(paths) == 0 {
		t.Fatal("expected at least one reviewed script")
	}
	paths[0] = "scripts/tampered.sh"
	if !IsReviewedScriptPath(dispatchableScripts[0]) {
		t.Fatal("editing the returned slice must not reach the door's list")
	}
	if IsReviewedScriptPath("scripts/tampered.sh") {
		t.Fatal("the door must not have taken the edit")
	}
}

// The pair that cannot be allowed to drift: every script the SHIPPED catalog
// dispatches must be on this list. The reverse is deliberately not asserted;
// review may admit a script before a task names it.
func TestTheShippedCatalogDispatchesOnlyReviewedScripts(t *testing.T) {
	var parsed struct {
		Tasks []Task `json:"tasks"`
	}
	if err := json.Unmarshal(catalog.Manifest(), &parsed); err != nil {
		t.Fatalf("reading the shipped catalog: %v", err)
	}
	if len(parsed.Tasks) == 0 {
		t.Fatal("the shipped catalog must carry tasks, or this test proves nothing")
	}
	dispatched := 0
	for _, task := range parsed.Tasks {
		for _, step := range task.Steps {
			if step.Program != DispatchShell || len(step.Args) == 0 {
				continue
			}
			dispatched++
			if !IsReviewedScriptPath(step.Args[0]) {
				t.Fatalf("task %q step %q dispatches %q, which is not on the reviewed list",
					task.Name, step.Name, step.Args[0])
			}
		}
	}
	if dispatched == 0 {
		t.Fatal("the shipped catalog must dispatch at least one script")
	}
}

// And the whole catalog must still load, which runs every door over it.
func TestTheShippedCatalogPassesTheScriptDoor(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
