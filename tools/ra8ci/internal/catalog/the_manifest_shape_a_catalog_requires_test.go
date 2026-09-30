// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// The manifest shape requireManifestFields insists on, before any value in it
// is judged. Every case below is this document with one thing taken away or
// written as the wrong kind, so a refusal names the field that is actually at
// fault rather than whatever the decoder tripped over first.
const wholeManifestTask = `{
	"name": "gate.ascii",
	"version": "1",
	"tier": "read-only",
	"scope": "repo",
	"os": ["linux"],
	"capabilities": [],
	"args_schema": {"positional": [], "flags": []},
	"deadline_seconds": 600,
	"board_policy": "none",
	"steps": [{"name": "scan", "program": "gate", "args": []}],
	"outputs": [],
	"retry": {"max_attempts": 1},
	"resource_hints": {}
}`

func manifestHolding(task string) []byte {
	return []byte(`{"schema_version": 1, "tasks": [` + task + `]}`)
}

// taskWithout rewrites the whole task with one top-level key renamed out of
// the way, which is how a field goes missing without disturbing the rest of
// the document's shape.
func taskWithout(t *testing.T, field string) string {
	t.Helper()
	replaced := strings.Replace(wholeManifestTask, `"`+field+`":`, `"not_`+field+`":`, 1)
	if replaced == wholeManifestTask {
		t.Fatalf("field %q does not appear in the whole manifest task", field)
	}
	return replaced
}

// taskWritingAs rewrites one field's whole value, so a list can be handed in
// as an object and an object as a list. Every field in the template sits on
// its own line, so the line is the unit to replace: scanning to the next comma
// would cut inside a value that holds one.
func taskWritingAs(t *testing.T, field, value string) string {
	t.Helper()
	marker := `"` + field + `":`
	lines := strings.Split(wholeManifestTask, "\n")
	for i, line := range lines {
		if !strings.HasPrefix(strings.TrimSpace(line), marker) {
			continue
		}
		rewritten := "\t" + marker + " " + value
		if strings.HasSuffix(strings.TrimSpace(line), ",") {
			rewritten += ","
		}
		lines[i] = rewritten
		return strings.Join(lines, "\n")
	}
	t.Fatalf("field %q does not appear on a line of its own in the whole manifest task", field)
	return ""
}

func TestAWholeManifestIsAccepted(t *testing.T) {
	if err := requireManifestFields(manifestHolding(wholeManifestTask)); err != nil {
		t.Fatalf("the whole manifest was refused: %v", err)
	}
}

func TestAManifestThatIsNotAnObjectIsRefused(t *testing.T) {
	for _, raw := range []string{``, `[]`, `{`, `"a string"`, `{"schema_version": }`} {
		if err := requireManifestFields([]byte(raw)); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("manifest %q was not refused as an invalid catalog: %v", raw, err)
		}
	}
}

func TestAManifestMissingATopLevelFieldNamesIt(t *testing.T) {
	for field, raw := range map[string]string{
		"schema_version": `{"tasks": []}`,
		"tasks":          `{"schema_version": 1}`,
	} {
		err := requireManifestFields([]byte(raw))
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("a manifest without %s was not refused: %v", field, err)
		}
		if !strings.Contains(err.Error(), field) {
			t.Fatalf("the refusal for a missing %s does not name it: %v", field, err)
		}
	}
}

func TestTasksWrittenAsAnythingButAListIsRefused(t *testing.T) {
	for _, written := range []string{`{}`, `"one"`, `7`, `null`} {
		raw := []byte(`{"schema_version": 1, "tasks": ` + written + `}`)
		if err := requireManifestFields(raw); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("tasks written as %s was not refused: %v", written, err)
		}
	}
}

// A list of the right kind can still hold the wrong thing, and that refusal
// belongs to tasks rather than to any one task's fields.
func TestAListOfTasksHoldingSomethingElseIsRefused(t *testing.T) {
	for _, written := range []string{`["a name"]`, `[7]`, `[[]]`} {
		raw := []byte(`{"schema_version": 1, "tasks": ` + written + `}`)
		err := requireManifestFields(raw)
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("tasks holding %s was not refused: %v", written, err)
		}
		if !strings.Contains(err.Error(), "tasks") {
			t.Fatalf("the refusal for tasks holding %s does not name tasks: %v", written, err)
		}
	}
}

func TestATaskMissingAnyDeclaredFieldNamesThatField(t *testing.T) {
	for _, field := range []string{
		"name", "version", "tier", "scope", "os", "capabilities", "args_schema",
		"deadline_seconds", "board_policy", "steps", "outputs", "retry", "resource_hints",
	} {
		err := requireManifestFields(manifestHolding(taskWithout(t, field)))
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("a task without %s was not refused: %v", field, err)
		}
		if !strings.Contains(err.Error(), field) {
			t.Fatalf("the refusal for a missing %s does not name it: %v", field, err)
		}
	}
}

func TestATaskFieldWrittenAsTheWrongKindIsRefused(t *testing.T) {
	for _, field := range []string{"os", "capabilities", "steps", "outputs"} {
		if err := requireManifestFields(manifestHolding(taskWritingAs(t, field, `{}`))); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s written as an object was not refused: %v", field, err)
		}
	}
	for _, field := range []string{"args_schema", "retry", "resource_hints"} {
		if err := requireManifestFields(manifestHolding(taskWritingAs(t, field, `[]`))); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s written as a list was not refused: %v", field, err)
		}
	}
}

func TestAnArgumentSchemaIsHeldToBothOfItsLists(t *testing.T) {
	cases := map[string]string{
		"not an object at all": `"positional"`,
		"missing positional":   `{"flags": []}`,
		"missing flags":        `{"positional": []}`,
		"positional as object": `{"positional": {}, "flags": []}`,
		"flags as object":      `{"positional": [], "flags": {}}`,
	}
	for name, written := range cases {
		err := requireManifestFields(manifestHolding(taskWritingAs(t, "args_schema", written)))
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("an args_schema %s was not refused: %v", name, err)
		}
	}
}

func TestARetryBlockWithoutItsAttemptCeilingIsRefused(t *testing.T) {
	err := requireManifestFields(manifestHolding(taskWritingAs(t, "retry", `{"backoff_seconds": 5}`)))
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a retry block without max_attempts was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "max_attempts") {
		t.Fatalf("the refusal does not name max_attempts: %v", err)
	}
	if err := requireManifestFields(manifestHolding(taskWritingAs(t, "retry", `{"max_attempts": "twice"}`))); err != nil {
		t.Fatalf("max_attempts is checked for presence here, not for kind: %v", err)
	}
}

// steps and args_schema are both declared as their kind above, so the only way
// left for either to fail its own decode is to hold something of the wrong
// kind inside a list of the right kind.
func TestAStepListHoldingSomethingElseIsRefused(t *testing.T) {
	for _, written := range []string{`["scan"]`, `[7]`, `[[]]`} {
		err := requireManifestFields(manifestHolding(taskWritingAs(t, "steps", written)))
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("steps holding %s was not refused: %v", written, err)
		}
		if !strings.Contains(err.Error(), "steps") {
			t.Fatalf("the refusal for steps holding %s does not name steps: %v", written, err)
		}
	}
}

func TestAStepMissingItsOwnFieldsIsRefused(t *testing.T) {
	cases := map[string]string{
		"name":    `[{"program": "gate", "args": []}]`,
		"program": `[{"name": "scan", "args": []}]`,
		"args":    `[{"name": "scan", "program": "gate"}]`,
	}
	for field, written := range cases {
		err := requireManifestFields(manifestHolding(taskWritingAs(t, "steps", written)))
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("a step without %s was not refused: %v", field, err)
		}
		if !strings.Contains(err.Error(), field) {
			t.Fatalf("the refusal for a step without %s does not name it: %v", field, err)
		}
	}
}

func TestAStepArgumentListWrittenAsAnObjectIsRefused(t *testing.T) {
	written := `[{"name": "scan", "program": "gate", "args": {}}]`
	if err := requireManifestFields(manifestHolding(taskWritingAs(t, "steps", written))); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a step whose args are an object was not refused: %v", err)
	}
}

// A second task is judged as whole as the first, so a manifest cannot smuggle
// a broken task in behind a sound one.
func TestASecondTaskIsHeldToTheSameShape(t *testing.T) {
	raw := []byte(`{"schema_version": 1, "tasks": [` + wholeManifestTask + `,` + taskWithout(t, "steps") + `]}`)
	err := requireManifestFields(raw)
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a broken second task was not refused: %v", err)
	}
	if !strings.Contains(err.Error(), "steps") {
		t.Fatalf("the refusal does not name the missing field: %v", err)
	}
}
