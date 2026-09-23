// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
)

func TestLoadReviewedTasks(t *testing.T) {
	c, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	names := c.Names()
	if len(names) != 73 {
		t.Fatalf("catalog has %d tasks, want 73", len(names))
	}
	deadlines := map[string]int{
		"format": 900, "format-check": 900, "lint-go": 1200, "test-go": 1800,
		"ascii": 1800, "unit-tests": 7200, "fuzz-sweep": 86400,
	}
	for name, deadline := range deadlines {
		task, found := c.Task(name)
		if !found || task.DeadlineSeconds != deadline || !task.IsSafeLocal() || !task.SupportsOS("linux") || task.SupportsOS("windows") {
			t.Fatalf("invalid reviewed task %q: %+v (found %t)", name, task, found)
		}
		if task.SupportsCurrentOS() != task.SupportsOS(runtime.GOOS) {
			t.Fatalf("current OS support disagrees for %q", name)
		}
		if err := task.ValidateArguments(nil); err != nil {
			t.Fatalf("task %q rejected empty arguments: %v", name, err)
		}
		if err := task.ValidateArguments([]string{"extra"}); err == nil {
			t.Fatalf("task %q accepted unreviewed arguments", name)
		}
	}
	for _, name := range []string{"hil-all", "bench-lock-selftest", "soup-upstream-refresh"} {
		if _, found := c.Task(name); found {
			t.Fatalf("task %q must remain outside local dispatch until its safety boundary is modeled", name)
		}
	}
	gate, found := c.Task("emulator-matrix")
	if !found || len(gate.Steps) != 1 || gate.Steps[0].Program != "bash" ||
		!reflect.DeepEqual(gate.Steps[0].Args, []string{"scripts/ci.sh", "--gate", "emulator-matrix"}) {
		t.Fatalf("CI gate task does not preserve exact argv: %+v", gate)
	}
	if _, found := c.Task("does-not-exist"); found {
		t.Fatal("unknown task was found")
	}
	if c.Digest() != strings.TrimSpace(string(embedded.Digest())) {
		t.Fatal("embedded catalog digest differs from manifest")
	}
}

func TestCatalogReturnsCopies(t *testing.T) {
	c, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	names := c.Names()
	originalFirst := names[0]
	names[0] = "mutated"
	task, _ := c.Task("format")
	task.OS[0] = "windows"
	task.Steps[0].Args[0] = "elsewhere"
	again, found := c.Task("format")
	if !found || again.OS[0] != "linux" || again.Steps[0].Args[0] != "scripts/checks/format_tree.sh" || c.Names()[0] != originalFirst {
		t.Fatalf("catalog was mutable through accessors: %+v", again)
	}
}

func TestParseRejectsUnknownAndDuplicateFields(t *testing.T) {
	tests := []string{
		`{"schema_version":1,"tasks":[],"surprise":true}`,
		`{"schema_version":1,"schema_version":1,"tasks":[]}`,
		`{"schema_version":1,"tasks":[{"name":"format","name":"format"}]}`,
		`{"schema_version":2,"tasks":[]}`,
		`{"schema_version":1,"tasks":[]}`,
		`{"schema_version":1,"tasks":[]} {}`,
	}
	for _, raw := range tests {
		t.Run(raw, func(t *testing.T) {
			_, err := Parse([]byte(raw), digestOf(t, []byte(raw)))
			if !errors.Is(err, ErrInvalidCatalog) {
				t.Fatalf("Parse error = %v, want invalid catalog", err)
			}
		})
	}
}

func TestParseRejectsDigestMismatch(t *testing.T) {
	_, err := Parse(embedded.Manifest(), strings.Repeat("0", 64))
	if !errors.Is(err, ErrDigestMismatch) {
		t.Fatalf("Parse error = %v, want digest mismatch", err)
	}
	_, err = Parse(embedded.Manifest(), "not a digest")
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("Parse malformed digest error = %v", err)
	}
}

func TestCanonicalJSONPreservesLargeInteger(t *testing.T) {
	canonical, err := CanonicalJSON([]byte(`{"z":9007199254740993,"a":1}`))
	if err != nil || string(canonical) != `{"a":1,"z":9007199254740993}` {
		t.Fatalf("canonical = %q, error = %v", canonical, err)
	}
}

func TestParseRequiresEveryDeclaredField(t *testing.T) {
	base := string(embedded.Manifest())
	mutations := []string{
		strings.Replace(base, `"outputs": [],`, ``, 1),
		strings.Replace(base, `"resource_hints": {}`, `"resource_hints": null`, 1),
		strings.Replace(base, `"positional": [],`, ``, 1),
		strings.Replace(base, `"max_attempts": 1`, `"max_attempts": null`, 1),
		strings.Replace(base, `"args": [`, `"args": null`, 1),
	}
	for index, mutation := range mutations {
		t.Run(string(rune('a'+index)), func(t *testing.T) {
			_, err := Parse([]byte(mutation), digestOf(t, []byte(mutation)))
			if !errors.Is(err, ErrInvalidCatalog) {
				t.Fatalf("mutation %d: error = %v", index, err)
			}
		})
	}
}

func TestParseRejectsUnsupportedTaskBehavior(t *testing.T) {
	base := string(embedded.Manifest())
	mutations := []string{
		strings.Replace(base, `"board_policy": "none"`, `"board_policy": "required"`, 1),
		strings.Replace(base, `"max_attempts": 1`, `"max_attempts": 2`, 1),
		strings.Replace(base, `"deadline_seconds": 900`, `"deadline_seconds": 0`, 1),
		strings.Replace(base, `"linux"`, `"darwin"`, 1),
		strings.Replace(base, `"program": "bash"`, `"program": ""`, 1),
		strings.Replace(base, `"tier": "required"`, `"tier": "unknown"`, 1),
		strings.Replace(base, `"scope": "safe-local-read-only"`, `"scope": "unknown"`, 1),
		strings.Replace(base, `"capabilities": []`, `"capabilities": ["board"]`, 1),
	}
	for index, mutation := range mutations {
		t.Run(string(rune('a'+index)), func(t *testing.T) {
			_, err := Parse([]byte(mutation), digestOf(t, []byte(mutation)))
			if !errors.Is(err, ErrInvalidCatalog) {
				t.Fatalf("mutation %d: error = %v", index, err)
			}
		})
	}
}

func TestVerifyCheckout(t *testing.T) {
	root := t.TempDir()
	if err := os.Mkdir(filepath.Join(root, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	base := filepath.Join(root, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(base, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "tasks.json"), embedded.Manifest(), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "sha256.txt"), embedded.Digest(), 0600); err != nil {
		t.Fatal(err)
	}
	verified, err := VerifyCheckout(root)
	if err != nil || verified != root {
		t.Fatalf("VerifyCheckout = %q, %v", verified, err)
	}
	if err := os.WriteFile(filepath.Join(base, "tasks.json"), []byte(`{"schema_version":1,"tasks":[]}`), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := VerifyCheckout(root); err == nil {
		t.Fatal("tampered checkout was accepted")
	}
	if _, err := VerifyCheckout(""); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("empty checkout error = %v", err)
	}
}

func TestVerifyCheckoutRejectsMissingFilesAndDigest(t *testing.T) {
	root := t.TempDir()
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("missing .git error = %v", err)
	}
	if err := os.Mkdir(filepath.Join(root, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	base := filepath.Join(root, "tools", "ra8ci", "catalog")
	if err := os.MkdirAll(base, 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("missing manifest error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(base, "tasks.json"), embedded.Manifest(), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrInvalidCheckout) {
		t.Fatalf("missing digest error = %v", err)
	}
	if err := os.WriteFile(filepath.Join(base, "sha256.txt"), []byte(strings.Repeat("0", 64)), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := VerifyCheckout(root); !errors.Is(err, ErrDigestMismatch) {
		t.Fatalf("wrong digest error = %v", err)
	}
}

func TestValidateTaskRejectsUnsafeIdentityAndHints(t *testing.T) {
	c, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	task, _ := c.Task("format")
	task.Name = "Format"
	if !errors.Is(ValidateTask(task), ErrInvalidCatalog) {
		t.Fatal("uppercase task name accepted")
	}
	task, _ = c.Task("format")
	task.ResourceHints.RAMBytes = -1
	if !errors.Is(ValidateTask(task), ErrInvalidCatalog) {
		t.Fatal("negative RAM hint accepted")
	}
	task, _ = c.Task("format")
	task.Steps[0].Args = append(task.Steps[0].Args, "bad\x00argument")
	if !errors.Is(ValidateTask(task), ErrInvalidCatalog) {
		t.Fatal("NUL argument accepted")
	}
}

func TestNilCatalog(t *testing.T) {
	var c *Catalog
	if c.Digest() != "" || c.Names() != nil {
		t.Fatal("nil catalog returned data")
	}
	if _, found := c.Task("format"); found {
		t.Fatal("nil catalog found a task")
	}
}

func digestOf(t *testing.T, raw []byte) string {
	t.Helper()
	canonical, err := CanonicalJSON(raw)
	if err != nil {
		return strings.Repeat("0", 64)
	}
	sum := sha256.Sum256(canonical)
	return hex.EncodeToString(sum[:])
}

func TestValidateHILTaskRequiresReviewedBoardAndObservationContract(t *testing.T) {
	catalog, err := Load()
	if err != nil {
		t.Fatal(err)
	}
	task, found := catalog.Task("format")
	if !found {
		t.Fatal("format fixture not found")
	}
	task.Scope = "hil"
	task.BoardPolicy = "exclusive"
	task.Steps = append(task.Steps, Step{Name: "observe", Program: "ra8ci", Args: []string{"internal-observe"}})
	task.HIL = &HILTask{BoardID: "ek-ra8d2", BoardModel: "EK-RA8D2",
		ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily: "uart-demo", Mode: "uart_scrape", ObservationStep: "observe", FlashRestoreSeconds: 10}
	if err := ValidateTask(task); err != nil {
		t.Fatalf("valid HIL definition rejected: %v", err)
	}
	copy := cloneTask(task)
	copy.HIL.Mode = "other"
	if task.HIL.Mode != "uart_scrape" {
		t.Fatal("task accessor exposed mutable HIL metadata")
	}
	invalid := []func(*Task){
		func(task *Task) { task.BoardPolicy = "none" },
		func(task *Task) { task.HIL = nil },
		func(task *Task) { task.HIL.ManifestPath = "docs/hil.conf" },
		func(task *Task) { task.HIL.BoardID = "../board" },
		func(task *Task) { task.HIL.Mode = "unknown" },
		func(task *Task) { task.HIL.ObservationStep = "missing" },
	}
	for index, edit := range invalid {
		broken := cloneTask(task)
		edit(&broken)
		if err := ValidateTask(broken); !errors.Is(err, ErrInvalidCatalog) {
			t.Errorf("invalid HIL mutation %d accepted: %v", index, err)
		}
	}
}
