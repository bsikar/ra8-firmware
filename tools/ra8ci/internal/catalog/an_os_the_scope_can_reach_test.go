// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

func osScopedTask(scope string, goos ...string) Task {
	return Task{Name: "gates", Scope: scope, OS: goos,
		Steps: []Step{{Name: "gate", Program: "bash", Args: []string{"scripts/ci.sh", "--fast"}}}}
}

func TestAWindowsScopedTaskDeclaringOnlyLinuxIsRefused(t *testing.T) {
	err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask("windows-vm", "linux"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
}

func TestALinuxVMScopedTaskDeclaringOnlyWindowsIsRefused(t *testing.T) {
	err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask("linux-vm", "windows"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
}

// runner is the third class the server pins, and it pins linux exactly as
// linux-vm does: store.agentForCertificate denies either whose declared OS is
// not linux.
func TestARunnerScopedTaskDeclaringOnlyWindowsIsRefused(t *testing.T) {
	err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask("runner", "windows"))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("want ErrInvalidCatalog, got %v", err)
	}
}

func TestTheScopeOSRefusalNamesTheTaskTheScopeAndBothOSReadings(t *testing.T) {
	err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask("windows-vm", "linux"))
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{`"gates"`, `"windows-vm"`, "windows", "linux"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q does not name %s", err, want)
		}
	}
}

func TestTheScopeOSRefusalSpellsEveryDeclaredOS(t *testing.T) {
	task := osScopedTask("windows-vm", "linux")
	task.OS = []string{"linux", "linux"}
	err := checkTheDeclaredOSReachesTheScopesHosts(task)
	if err == nil || strings.Count(err.Error(), "linux") < 2 {
		t.Fatalf("want every declared OS spelled out, got %v", err)
	}
}

func TestATaskDeclaringTheOSItsScopePinsIsAdmitted(t *testing.T) {
	for scope, pinned := range map[string]string{"runner": "linux", "linux-vm": "linux", "windows-vm": "windows"} {
		if err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask(scope, pinned)); err != nil {
			t.Fatalf("scope %q declaring %q: %v", scope, pinned, err)
		}
	}
}

// A definition meant for two host classes names both, and the one its own
// scope reaches is among them. Order must not matter.
func TestATaskDeclaringBothIsAdmittedUnderEveryPinnedScope(t *testing.T) {
	for _, scope := range []string{"runner", "linux-vm", "windows-vm"} {
		for _, declared := range [][]string{{"linux", "windows"}, {"windows", "linux"}} {
			if err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask(scope, declared...)); err != nil {
				t.Fatalf("scope %q declaring %v: %v", scope, declared, err)
			}
		}
	}
}

// Both safe-local scopes run wherever the agent already is, and
// store.ClaimAgentTask picks a task by the host's own OS, so either reading is
// a real answer. A hil task reaches a board through the commons rather than a
// host class. None of the three is judged here.
func TestAnUnpinnedScopeIsNotJudgedHere(t *testing.T) {
	for _, scope := range []string{"safe-local-read-only", "safe-local-write-working-tree", "hil"} {
		for _, declared := range [][]string{{"linux"}, {"windows"}} {
			if err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask(scope, declared...)); err != nil {
				t.Fatalf("scope %q declaring %v is not this door's question: %v", scope, declared, err)
			}
		}
	}
}

// An empty or unknown-valued OS list is ValidateTask's refusal, not this
// door's: it holds the list to linux, windows and no duplicate. This one must
// not claim that refusal for a scope it does not pin, and must still make its
// own for a scope it does.
func TestAnEmptyOSListIsLeftToTheIdentityRule(t *testing.T) {
	if err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask("safe-local-read-only")); err != nil {
		t.Fatalf("an unpinned scope with no OS is not this door's question: %v", err)
	}
	if err := checkTheDeclaredOSReachesTheScopesHosts(osScopedTask("windows-vm")); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a pinned scope naming no OS reaches no host: %v", err)
	}
}

func TestHostOSAScopePinsAnswersForEveryScopeTheCatalogKnows(t *testing.T) {
	want := map[string]string{"runner": "linux", "linux-vm": "linux", "windows-vm": "windows"}
	for _, scope := range []string{"safe-local-read-only", "safe-local-write-working-tree",
		"runner", "linux-vm", "windows-vm", "hil"} {
		goos, pinned := HostOSAScopePins(scope)
		expected, shouldPin := want[scope]
		if pinned != shouldPin || goos != expected {
			t.Fatalf("scope %q: got (%q, %v), want (%q, %v)", scope, goos, pinned, expected, shouldPin)
		}
	}
}

// Every OS this door pins must be one ValidateTask would admit in an OS list,
// or the door would refuse a definition no valid task could ever satisfy.
func TestEveryPinnedOSIsOneAValidTaskMayDeclare(t *testing.T) {
	for scope, pinned := range hostOSAScopePins {
		task := osScopedTask(scope, pinned)
		task.Tier = "required"
		task.DeadlineSeconds = 60
		task.BoardPolicy = "none"
		task.Version = 1
		task.Retry = RetryPolicy{MaxAttempts: 1}
		if err := ValidateTask(task); err != nil {
			t.Fatalf("scope %q pins %q, which ValidateTask will not admit: %v", scope, pinned, err)
		}
	}
}

// The shipped catalog must already satisfy the rule, or admission breaks on
// the definitions this binary carries.
func TestTheShippedCatalogDeclaresTheOSEachScopeReaches(t *testing.T) {
	loaded, err := Load()
	if err != nil {
		t.Fatalf("load shipped catalog: %v", err)
	}
	for _, name := range loaded.Names() {
		task, _ := loaded.Task(name)
		if err := checkTheDeclaredOSReachesTheScopesHosts(task); err != nil {
			t.Fatalf("shipped task %q: %v", name, err)
		}
	}
}
