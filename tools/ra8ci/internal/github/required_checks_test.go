// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"reflect"
	"sort"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The gate is the one thing in #1481 that can hold every pull request in the
// repository, so these tests pin what the plan refuses to propose at least as
// hard as what it proposes.

func loadCatalogNames(t *testing.T) []string {
	t.Helper()
	loaded, err := catalog.Load()
	if err != nil {
		t.Fatalf("catalog.Load: %v", err)
	}
	names := loaded.Names()
	if len(names) == 0 {
		t.Fatal("embedded catalog carries no tasks")
	}
	return names
}

func TestAuthoritativePlanRequiresEveryCatalogTask(t *testing.T) {
	names := loadCatalogNames(t)
	plan, err := PlanRequiredChecks(ModeAuthoritative, names, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	if len(plan.Add) != len(names) {
		t.Fatalf("Add has %d contexts for %d tasks", len(plan.Add), len(names))
	}
	if len(plan.Remove) != 0 || len(plan.Keep) != 0 || len(plan.Foreign) != 0 {
		t.Fatalf("a gate requiring nothing today should only add: %+v", plan)
	}
	if plan.NoChange() {
		t.Fatal("NoChange on a plan that adds every context")
	}
	for _, task := range names {
		want, err := CheckRunName(ModeAuthoritative, task)
		if err != nil {
			t.Fatalf("CheckRunName(%q): %v", task, err)
		}
		if sort.SearchStrings(plan.Add, want) == len(plan.Add) || !contains(plan.Add, want) {
			t.Fatalf("Add does not carry %q", want)
		}
	}
}

// A shadow run reports neutral whatever the task did, and branch protection
// reads neutral as satisfying a required check. Requiring one buys a gate that
// cannot fail.
func TestShadowPlanProposesNoRequiredCheck(t *testing.T) {
	names := loadCatalogNames(t)
	plan, err := PlanRequiredChecks(ModeShadow, names, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	if len(plan.Add) != 0 {
		t.Fatalf("shadow plan adds %v", plan.Add)
	}
	if !plan.NoChange() {
		t.Fatalf("shadow plan over an empty gate should move nothing: %+v", plan)
	}
}

// A deployment in shadow mode publishes no authoritative run, so a required
// authoritative context would never be answered and every pull request would
// wait on it.
func TestShadowPlanReleasesAnAuthoritativeContextNothingWillAnswer(t *testing.T) {
	names := loadCatalogNames(t)
	required, err := CheckRunName(ModeAuthoritative, names[0])
	if err != nil {
		t.Fatalf("CheckRunName: %v", err)
	}
	plan, err := PlanRequiredChecks(ModeShadow, names, []string{required})
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	if !reflect.DeepEqual(plan.Remove, []string{required}) {
		t.Fatalf("Remove = %v, want %v", plan.Remove, []string{required})
	}
	if len(plan.Keep) != 0 || len(plan.Add) != 0 {
		t.Fatalf("shadow plan kept or added something: %+v", plan)
	}
}

func TestAShadowContextIsRemovedInEitherMode(t *testing.T) {
	names := loadCatalogNames(t)
	shadow, err := CheckRunName(ModeShadow, names[0])
	if err != nil {
		t.Fatalf("CheckRunName: %v", err)
	}
	for _, mode := range []CheckRunMode{ModeShadow, ModeAuthoritative} {
		plan, err := PlanRequiredChecks(mode, names, []string{shadow})
		if err != nil {
			t.Fatalf("PlanRequiredChecks(%s): %v", mode, err)
		}
		if !contains(plan.Remove, shadow) {
			t.Fatalf("%s plan leaves the shadow context required: %+v", mode, plan)
		}
		if contains(plan.Keep, shadow) || contains(plan.Foreign, shadow) {
			t.Fatalf("%s plan classified the shadow context as something to leave alone: %+v", mode, plan)
		}
	}
}

// The plan owns two namespaces and nothing else. An Actions job on the same
// gate is reported so the operator sees the whole list, never proposed for
// removal.
func TestAContextThisPlaneDoesNotOwnIsNeverRemoved(t *testing.T) {
	names := loadCatalogNames(t)
	foreign := []string{
		"build (ubuntu-latest)",
		"ra8ci-lint",
		"ra8cifake / build",
		"CodeQL",
	}
	for _, mode := range []CheckRunMode{ModeShadow, ModeAuthoritative} {
		plan, err := PlanRequiredChecks(mode, names, foreign)
		if err != nil {
			t.Fatalf("PlanRequiredChecks(%s): %v", mode, err)
		}
		want := append([]string(nil), foreign...)
		sort.Strings(want)
		if !reflect.DeepEqual(plan.Foreign, want) {
			t.Fatalf("%s Foreign = %v, want %v", mode, plan.Foreign, want)
		}
		if len(plan.Remove) != 0 {
			t.Fatalf("%s plan proposes removing a context it does not own: %v", mode, plan.Remove)
		}
	}
}

// A required name no reviewed task answers to is a gate nothing will ever
// satisfy, so it is named for removal rather than left in place.
func TestARequiredNameNoTaskAnswersToIsRemoved(t *testing.T) {
	names := loadCatalogNames(t)
	stale := checkRunNamespace + checkRunNameSeparator + "task-that-was-renamed"
	live, err := CheckRunName(ModeAuthoritative, names[0])
	if err != nil {
		t.Fatalf("CheckRunName: %v", err)
	}
	plan, err := PlanRequiredChecks(ModeAuthoritative, names, []string{stale, live})
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	if !reflect.DeepEqual(plan.Remove, []string{stale}) {
		t.Fatalf("Remove = %v, want %v", plan.Remove, []string{stale})
	}
	if !reflect.DeepEqual(plan.Keep, []string{live}) {
		t.Fatalf("Keep = %v, want %v", plan.Keep, []string{live})
	}
	if contains(plan.Add, live) {
		t.Fatal("a context already required was proposed again")
	}
}

func TestPlanIsOrderedSoTwoPassesAreDiffable(t *testing.T) {
	names := loadCatalogNames(t)
	reversed := append([]string(nil), names...)
	for i, j := 0, len(reversed)-1; i < j; i, j = i+1, j-1 {
		reversed[i], reversed[j] = reversed[j], reversed[i]
	}
	first, err := PlanRequiredChecks(ModeAuthoritative, names, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	second, err := PlanRequiredChecks(ModeAuthoritative, reversed, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	if !reflect.DeepEqual(first, second) {
		t.Fatal("the same tasks in another order produced a different plan")
	}
	if !sort.StringsAreSorted(first.Add) {
		t.Fatalf("Add is not sorted: %v", first.Add)
	}
}

func TestPlanRefusesInputItCannotRead(t *testing.T) {
	names := loadCatalogNames(t)
	cases := []struct {
		name     string
		mode     CheckRunMode
		tasks    []string
		required []string
		want     error
	}{
		{"unknown mode", CheckRunMode(7), names, nil, ErrInvalidCheckRunMode},
		{"no tasks", ModeAuthoritative, nil, nil, ErrNoRequiredCheckTasks},
		{"task name outside the catalog rule", ModeAuthoritative, []string{"Build Firmware"}, nil, ErrRequiredCheckTaskInvalid},
		{"same task twice", ModeAuthoritative, []string{names[0], names[0]}, nil, ErrRequiredCheckSetAmbiguous},
		{"empty context", ModeAuthoritative, names, []string{""}, ErrRequiredCheckContextInvalid},
		{"padded context", ModeAuthoritative, names, []string{" ra8ci / build "}, ErrRequiredCheckContextInvalid},
		{"same context twice", ModeAuthoritative, names, []string{"CodeQL", "CodeQL"}, ErrRequiredCheckSetAmbiguous},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			plan, err := PlanRequiredChecks(test.mode, test.tasks, test.required)
			if !errors.Is(err, test.want) {
				t.Fatalf("err = %v, want %v", err, test.want)
			}
			if !reflect.DeepEqual(plan, RequiredCheckPlan{}) {
				t.Fatalf("a refused plan carried contexts: %+v", plan)
			}
		})
	}
}

// An unknown mode is refused on the mode alone, before the task set is read, so
// a caller bug does not depend on what the catalog happens to carry.
func TestAnUnknownModeIsRefusedBeforeTheTasksAreRead(t *testing.T) {
	_, err := PlanRequiredChecks(CheckRunMode(9), []string{"NOT A TASK NAME"}, nil)
	if !errors.Is(err, ErrInvalidCheckRunMode) {
		t.Fatalf("err = %v, want ErrInvalidCheckRunMode", err)
	}
}

func TestNoChangeReportsOnlyWhenNothingMoves(t *testing.T) {
	names := loadCatalogNames(t)
	required := make([]string, 0, len(names))
	for _, task := range names {
		name, err := CheckRunName(ModeAuthoritative, task)
		if err != nil {
			t.Fatalf("CheckRunName(%q): %v", task, err)
		}
		required = append(required, name)
	}
	plan, err := PlanRequiredChecks(ModeAuthoritative, names, required)
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	if !plan.NoChange() {
		t.Fatalf("a gate that already matches should move nothing: %+v", plan)
	}
	if len(plan.Keep) != len(names) {
		t.Fatalf("Keep has %d contexts for %d tasks", len(plan.Keep), len(names))
	}
	plan.Remove = append(plan.Remove, required[0])
	if plan.NoChange() {
		t.Fatal("NoChange with a removal pending")
	}
}

// Every planned context has to be a name a publisher actually posts, or branch
// protection waits on a run that never arrives.
func TestEveryPlannedContextIsOneAPublisherPosts(t *testing.T) {
	names := loadCatalogNames(t)
	plan, err := PlanRequiredChecks(ModeAuthoritative, names, nil)
	if err != nil {
		t.Fatalf("PlanRequiredChecks: %v", err)
	}
	const head = "0123456789abcdef0123456789abcdef01234567"
	posted := make(map[string]bool, len(names))
	for _, task := range names {
		run, err := NewTaskCheckRun(ModeAuthoritative, task, head, "succeeded")
		if err != nil {
			t.Fatalf("NewTaskCheckRun(%q): %v", task, err)
		}
		posted[run.Name] = true
	}
	for _, context := range plan.Add {
		if !posted[context] {
			t.Fatalf("plan requires %q, which no publisher posts", context)
		}
	}
}

func contains(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}
