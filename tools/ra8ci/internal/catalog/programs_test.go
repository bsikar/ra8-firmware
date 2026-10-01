// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"

	embedded "github.com/bsikar/ra8-firmware/tools/ra8ci/catalog"
)

func TestEveryReviewedTaskDispatchesThroughTheSeam(t *testing.T) {
	definitions, err := Load()
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	shells, tools := 0, 0
	for _, name := range definitions.Names() {
		task, found := definitions.Task(name)
		if !found {
			t.Fatalf("task %q vanished between listing and lookup", name)
		}
		if err := ValidateTaskDispatch(task); err != nil {
			t.Fatalf("task %q: %v", name, err)
		}
		for _, step := range task.Steps {
			if IsFrontDoorProgram(step.Program) {
				t.Fatalf("task %q step %q dispatches through %q", name, step.Name, step.Program)
			}
			if _, named := ToolProgram(step.Program); named {
				tools++
				continue
			}
			shells++
			if step.Program != DispatchShell {
				t.Fatalf("task %q step %q names %q", name, step.Name, step.Program)
			}
		}
	}
	if shells == 0 || tools == 0 {
		t.Fatalf("expected both dispatch shapes in the reviewed catalog, got %d shell and %d tool steps", shells, tools)
	}
}

func TestFrontDoorProgramsAreRefusedByName(t *testing.T) {
	for _, program := range FrontDoorPrograms() {
		step := Step{Name: "gate", Program: program, Args: []string{"quality::local::gate", "lint-go"}}
		err := ValidateStepDispatch(step)
		if !errors.Is(err, ErrFrontDoorProgram) {
			t.Fatalf("program %q: want ErrFrontDoorProgram, got %v", program, err)
		}
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("program %q: refusal must stay an invalid-catalog error, got %v", program, err)
		}
		if !strings.Contains(err.Error(), program) {
			t.Fatalf("refusal must name the front door, got %v", err)
		}
	}
}

func TestShellStepMustDispatchAReviewedScript(t *testing.T) {
	cases := map[string]Step{
		"no arguments at all":    {Name: "gate", Program: DispatchShell},
		"a command string":       {Name: "gate", Program: DispatchShell, Args: []string{"-c", "scripts/ci.sh --gate tidy"}},
		"an absolute script":     {Name: "gate", Program: DispatchShell, Args: []string{"/etc/ci.sh"}},
		"traversal":              {Name: "gate", Program: DispatchShell, Args: []string{"../../scripts/ci.sh"}},
		"not a script":           {Name: "gate", Program: DispatchShell, Args: []string{"scripts/ci"}},
		"a shell metacharacter":  {Name: "gate", Program: DispatchShell, Args: []string{"scripts/ci.sh;rm -rf /"}},
		"the program as a path":  {Name: "gate", Program: "scripts/ci.sh", Args: []string{"--gate", "tidy"}},
		"an unreviewed program":  {Name: "gate", Program: "python3", Args: []string{"scripts/ci.py"}},
		"an empty argument":      {Name: "gate", Program: DispatchShell, Args: []string{"scripts/ci.sh", ""}},
		"a control character":    {Name: "gate", Program: DispatchShell, Args: []string{"scripts/ci.sh", "--gate\ntidy"}},
		"no program":             {Name: "gate"},
		"a padded program":       {Name: "gate", Program: " bash", Args: []string{"scripts/ci.sh"}},
		"an unnamed ra8ci tool":  {Name: "gate", Program: ToolProgramPrefix},
		"a shouted ra8ci tool":   {Name: "gate", Program: ToolProgramPrefix + "No-Null"},
		"a windows-shaped path":  {Name: "gate", Program: `scripts\ci.sh`},
		"a bare dot script":      {Name: "gate", Program: DispatchShell, Args: []string{".sh"}},
		"an empty path segment":  {Name: "gate", Program: DispatchShell, Args: []string{"scripts//ci.sh"}},
		"a flag posing as path":  {Name: "gate", Program: DispatchShell, Args: []string{"-i.sh"}},
		"a deeply buried script": {Name: "gate", Program: DispatchShell, Args: []string{strings.Repeat("a/", MaxScriptPathSegments) + "ci.sh"}},
	}
	for name, step := range cases {
		if err := ValidateStepDispatch(step); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s: want a refusal, got %v", name, err)
		}
	}
	accepted := []Step{
		{Name: "gate", Program: DispatchShell, Args: []string{"scripts/ci.sh", "--gate", "tidy"}},
		{Name: "format", Program: DispatchShell, Args: []string{"scripts/checks/format_tree.sh", "--check"}},
		{Name: "selftest", Program: ToolProgramPrefix + "no-null", Args: []string{"--selftest"}},
		{Name: "scan", Program: ToolProgramPrefix + "no-null"},
	}
	for _, step := range accepted {
		if err := ValidateStepDispatch(step); err != nil {
			t.Fatalf("step %q: %v", step.Name, err)
		}
	}
}

func TestParseRefusesAManifestThatGoesThroughTheFrontDoor(t *testing.T) {
	mutated := strings.Replace(
		string(embedded.Manifest()),
		"\"program\": \"bash\",\n          \"args\": [\n            \"scripts/ci.sh\",\n            \"--gate\",\n            \"lint-go\"",
		"\"program\": \"just\",\n          \"args\": [\n            \"quality::local::gate\",\n            \"lint-go\"",
		1,
	)
	if mutated == string(embedded.Manifest()) {
		t.Fatal("mutation did not apply; the reviewed lint-go step changed shape")
	}
	if _, err := Parse([]byte(mutated), digestOf(t, []byte(mutated))); !errors.Is(err, ErrFrontDoorProgram) {
		t.Fatalf("want ErrFrontDoorProgram from the review boundary, got %v", err)
	}
}

func TestReviewedCatalogNamesNoFrontDoorAnywhere(t *testing.T) {
	var source struct {
		Tasks []Task `json:"tasks"`
	}
	if err := json.Unmarshal(embedded.Manifest(), &source); err != nil {
		t.Fatalf("decode manifest: %v", err)
	}
	if len(source.Tasks) == 0 {
		t.Fatal("no tasks decoded")
	}
	for _, task := range source.Tasks {
		for _, step := range task.Steps {
			for _, arg := range step.Args {
				if IsFrontDoorProgram(arg) {
					t.Fatalf("task %q step %q passes %q on argv", task.Name, step.Name, arg)
				}
			}
		}
	}
}

func TestDispatchRulesDoNotReachPersistedTaskRevalidation(t *testing.T) {
	// ValidateTask re-checks a task that was already admitted against a
	// reviewed digest, on paths that never see the manifest again. The seam is
	// a review rule, so it must not retroactively refuse such a task.
	task := Task{
		Name: "fixture", Version: 1, Tier: "required", Scope: "safe-local-read-only",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "none",
		Steps: []Step{{Name: "observe", Program: "echo", Args: []string{"hello"}}},
		Retry: RetryPolicy{MaxAttempts: 1},
	}
	if err := ValidateTask(task); err != nil {
		t.Fatalf("runtime re-validation must stay unchanged: %v", err)
	}
	if err := ValidateTaskDispatch(task); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the same task must fail review, got %v", err)
	} else if !strings.Contains(err.Error(), "fixture") {
		t.Fatalf("refusal must name the task, got %v", err)
	}
}

func TestValidScriptPathBounds(t *testing.T) {
	if !ValidScriptPath("scripts/ci.sh") || !ValidScriptPath("scripts/checks/format_tree.sh") {
		t.Fatal("reviewed script paths must be accepted")
	}
	long := "scripts/" + strings.Repeat("a", MaxScriptPathBytes) + ".sh"
	for _, path := range []string{"", ".sh", "scripts/ci.sh/", "./scripts/ci.sh", "scripts/../ci.sh", long, "scripts/ci.SH"} {
		if ValidScriptPath(path) {
			t.Fatalf("path %q must be refused", path)
		}
	}
}
