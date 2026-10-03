// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

// Every door in this package admits before it refuses: a step naming no
// arguments, a script that signed up to no contract, a tool absent from the
// reviewed tables. Those admissions are deliberate, and untested they are
// indistinguishable from a rule that silently never fires. These pin them,
// alongside the two refusals that sit next to them.

// A step with no arguments names no script, so every script door hands it
// straight back rather than reading step.Args[0] off an empty slice.
func TestAStepNamingNoArgumentsPassesEveryScriptDoor(t *testing.T) {
	bare := Step{Name: "bare", Program: "bash"}
	for name, door := range map[string]func(Step) error{
		"the ignored-option door": checkNoScriptOptionIsOneTheModeIgnores,
		"the named-twice door":    checkNoScriptOptionIsNamedTwice,
		"the companion door":      checkAScriptStepNamesEveryCompanionItNeeds,
		"the parsed-option door":  checkScriptOptionsAreOnesTheScriptParses,
		"the accepted-value door": checkScriptOptionValuesAreOnesTheScriptAccepts,
	} {
		if err := door(bare); err != nil {
			t.Fatalf("%s refused a step naming no arguments: %v", name, err)
		}
	}
}

// A script nothing reviewed is admitted on its path alone. The rule extends
// where a contract is stated rather than inventing a shell contract no script
// signed up to.
func TestAScriptWithNoStatedContractPassesEveryScriptDoor(t *testing.T) {
	unreviewed := Step{
		Name:    "unreviewed",
		Program: "bash",
		Args:    []string{"scripts/nothing-reviewed-this.sh", "--whatever", "--twice", "--twice"},
	}
	for name, door := range map[string]func(Step) error{
		"the ignored-option door": checkNoScriptOptionIsOneTheModeIgnores,
		"the named-twice door":    checkNoScriptOptionIsNamedTwice,
		"the companion door":      checkAScriptStepNamesEveryCompanionItNeeds,
		"the parsed-option door":  checkScriptOptionsAreOnesTheScriptParses,
		"the accepted-value door": checkScriptOptionValuesAreOnesTheScriptAccepts,
	} {
		if err := door(unreviewed); err != nil {
			t.Fatalf("%s refused a script it never reviewed: %v", name, err)
		}
	}
}

// The bound-argument door walks every step and skips the ones it cannot judge:
// a program that is not a tool at all, and a tool absent from the reviewed
// flag table. Neither is silently accepted elsewhere; both are refused by the
// doors that own that decision.
func TestTheBoundArgumentDoorSkipsAStepItCannotJudge(t *testing.T) {
	withAPositional := ArgsSchema{Positional: []string{"path"}}
	for name, step := range map[string]Step{
		"a script step":      {Name: "script", Program: "bash", Args: []string{"scripts/x.sh"}},
		"an unreviewed tool": {Name: "tool", Program: "ra8ci:nothing-reviewed-this"},
	} {
		task := Task{Name: "t", ArgsSchema: withAPositional, Steps: []Step{step}}
		if err := checkBoundArgumentsAreOnesTheToolTakes(task); err != nil {
			t.Fatalf("the bound-argument door judged %s: %v", name, err)
		}
	}
}

// A task declaring nothing to bind never reaches the per-step walk at all.
func TestTheBoundArgumentDoorPassesATaskThatBindsNothing(t *testing.T) {
	task := Task{
		Name:  "t",
		Steps: []Step{{Name: "tool", Program: "ra8ci:ascii", Args: []string{"-selftest"}}},
	}
	if err := checkBoundArgumentsAreOnesTheToolTakes(task); err != nil {
		t.Fatalf("a task binding nothing was judged: %v", err)
	}
}

// The flag door draws the opposite line: a dispatched tool must state the
// options it parses, so a tool absent from the reviewed table is refused
// rather than admitted on its name.
func TestADispatchedToolStatingNoReviewedFlagsIsRefused(t *testing.T) {
	step := Step{Name: "tool", Program: "ra8ci:nothing-reviewed-this", Args: []string{"-selftest"}}
	err := checkToolFlagsAreOnesTheToolParses(step, "ra8ci:nothing-reviewed-this")
	if !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a tool stating no reviewed flags was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "states no reviewed flags") {
		t.Fatalf("the refusal does not name what is missing: %v", err)
	}
	if err := checkToolFlagsAreOnesTheToolParses(step, "ra8ci:ascii"); err != nil {
		t.Fatalf("a reviewed tool was refused: %v", err)
	}
}

// BindArguments judges the schema before it binds anything, so a task whose
// own declaration is broken refuses at the schema rather than producing argv
// from it.
func TestBindingRefusesABrokenSchemaBeforeItBindsAnything(t *testing.T) {
	for name, schema := range map[string]ArgsSchema{
		"a name declared twice":    {Positional: []string{"path"}, Flags: []string{"path"}},
		"an invalid argument name": {Flags: []string{"Not A Name"}},
		"an empty argument name":   {Positional: []string{""}},
	} {
		argv, err := BindArguments(schema, map[string]string{})
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%s was bound rather than refused: %v", name, err)
		}
		if argv != nil {
			t.Fatalf("%s produced argv %q on the way to a refusal", name, argv)
		}
	}
}

func TestAScriptPathOutsidePrintableASCIIIsRefused(t *testing.T) {
	for name, path := range map[string]string{
		"a control character": "scripts/ci\x01.sh",
		"a tab":               "scripts/ci\t.sh",
		"a newline":           "scripts/ci\n.sh",
		"a non-ASCII rune":    "scripts/café.sh",
		"a deleted byte":      "scripts/ci\x7f.sh",
	} {
		if ValidScriptPath(path) {
			t.Fatalf("a script path holding %s was accepted", name)
		}
	}
	if !ValidScriptPath("scripts/ci/gate.sh") {
		t.Fatal("an ordinary script path was refused")
	}
}

// The same read refuses anything that is not a regular file, before its size
// is weighed, because a directory or a device has no size worth comparing.
func TestACheckoutPathThatIsNotARegularFileIsRefused(t *testing.T) {
	directory := t.TempDir()
	_, err := readCheckoutFile(directory, maxReadableManifestBytes)
	if err == nil || !strings.Contains(err.Error(), "not a regular file") {
		t.Fatalf("a directory was not refused as a non-regular file: %v", err)
	}
}
