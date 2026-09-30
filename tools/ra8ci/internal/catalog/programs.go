// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"fmt"
	"strings"
)

// The dispatch seam. A reviewed task reaches its work through exactly two
// shapes: an ra8ci tool the executor runs in process, or the pinned shell
// running a reviewed script that lives in the verified checkout. Everything
// else is refused at catalog review time.
//
// just is the human front door and the plane never goes through it. A front
// door recipe exists to make a developer's machine convenient: it is free to
// pick a devcontainer, re-exec itself, or change what it delegates to, none of
// which a reviewed digest can pin. It also puts a second program on the runner
// guest whose absence reads as a gate failure rather than a missing tool. The
// gate script is the contract; the recipe is a convenience over it.
//
// The security property is the SHAPE, the same argument args.go makes: the
// program is fixed, the script is one argv element, and no command ever exists
// as a string for a shell to re-split, expand or chain. A step that named bash
// with -c would hand back exactly the concatenated command this seam exists to
// keep away from a privileged boundary, so the first argument to the shell must
// be a script path and nothing else.
const (
	// ToolProgramPrefix marks a step the executor runs in process.
	ToolProgramPrefix = "ra8ci:"
	// DispatchShell is the only interpreter a reviewed step may name.
	DispatchShell = "bash"
	// MaxProgramBytes bounds a reviewed program name.
	MaxProgramBytes = 96
	// MaxToolNameBytes bounds the tool half of an ra8ci: program.
	MaxToolNameBytes = 64
	// MaxScriptPathBytes bounds the script a step dispatches.
	MaxScriptPathBytes = 256
	// MaxScriptPathSegments bounds how deep that script may sit.
	MaxScriptPathSegments = 12
	// ScriptPathSuffix is the only script extension a reviewed step dispatches.
	ScriptPathSuffix = ".sh"
)

// ErrFrontDoorProgram reports a task that dispatches through an entry point
// meant for a person at a terminal rather than through the reviewed script.
var ErrFrontDoorProgram = errors.New("task dispatches through a human front door")

// frontDoorPrograms are named so the refusal says why, rather than only that
// the program was not bash. Deliberately the human and build-orchestration
// front doors this repository actually has; a task that genuinely needs a
// compiler or a build system reaches it inside its reviewed gate script, where
// the pinned toolchain is set up, rather than naming it here.
//
// zig is on this list for the same reason make and ninja are, and it is the
// one worth spelling out because it does not look like a front door. `zig
// build` reads build.zig out of the checkout and decides from there what to
// compile, with what flags, against which dependencies: the step would pin the
// word "zig" and nothing about the work. That is the property a reviewed
// digest exists to hold. It is also a second toolchain on the runner guest
// whose absence answers as a gate failure rather than a missing tool, exactly
// the confusion the just entry describes. A Zig task reaches the compiler
// inside its reviewed script, where the toolchain version is set up and the
// build line is reviewed text, the same way every C gate reaches its own
// compiler today.
var frontDoorPrograms = []string{"just", "make", "gmake", "cmake", "ninja", "zig"}

// IsFrontDoorProgram reports whether a program is a human front door.
func IsFrontDoorProgram(program string) bool {
	for _, candidate := range frontDoorPrograms {
		if program == candidate {
			return true
		}
	}
	return false
}

// FrontDoorPrograms returns the reviewed front-door names.
func FrontDoorPrograms() []string {
	return append([]string(nil), frontDoorPrograms...)
}

// ToolProgram returns the ra8ci tool a program names, if it names one.
func ToolProgram(program string) (string, bool) {
	if !strings.HasPrefix(program, ToolProgramPrefix) {
		return "", false
	}
	return strings.TrimPrefix(program, ToolProgramPrefix), true
}

// ValidScriptPath reports whether a path is a reviewed script inside the
// checkout: relative, slash separated, no traversal, no shell metacharacter,
// and unmistakably a script rather than a flag.
func ValidScriptPath(path string) bool {
	if path == "" || len(path) > MaxScriptPathBytes || !strings.HasSuffix(path, ScriptPathSuffix) {
		return false
	}
	if strings.HasPrefix(path, "/") || strings.HasPrefix(path, "-") || strings.ContainsAny(path, shellMetacharacters) {
		return false
	}
	if !printableASCII(path) {
		return false
	}
	segments := strings.Split(path, "/")
	if len(segments) > MaxScriptPathSegments {
		return false
	}
	for _, segment := range segments {
		if segment == "" || segment == "." || segment == ".." {
			return false
		}
	}
	return len(segments[len(segments)-1]) > len(ScriptPathSuffix)
}

// ValidateStepDispatch is the reviewed-dispatch rule for one step. It is a
// catalog review rule, applied where a manifest is admitted, not a re-check of
// a task already persisted against a reviewed digest.
func ValidateStepDispatch(step Step) error {
	program := step.Program
	switch {
	case program == "" || len(program) > MaxProgramBytes:
		return fmt.Errorf("%w: step %q names no reviewed program", ErrInvalidCatalog, step.Name)
	case strings.TrimSpace(program) != program || !printableASCII(program):
		return fmt.Errorf("%w: step %q names an unreadable program", ErrInvalidCatalog, step.Name)
	}
	if err := validateDispatchArgs(step); err != nil {
		return err
	}
	if tool, named := ToolProgram(program); named {
		if !validName(tool) || len(tool) > MaxToolNameBytes {
			return fmt.Errorf("%w: step %q names an invalid ra8ci tool %q", ErrInvalidCatalog, step.Name, tool)
		}
		if err := checkToolProgramExists(step, tool); err != nil {
			return err
		}
		if err := checkToolFlagsAreOnesTheToolParses(step, program); err != nil {
			return err
		}
		if err := checkASelftestStepNamesNothingElse(step, program); err != nil {
			return err
		}
		if err := checkFileArgumentsAreOnesTheToolReads(step, program); err != nil {
			return err
		}
		if err := checkAnAllStepNamesTheWholeTree(step, program); err != nil {
			return err
		}
		if err := checkToolFlagValuesAreOnesTheToolAccepts(step, program); err != nil {
			return err
		}
		// Beside the file door, which asks whether the tool reads a path at
		// all: this one asks how many it can read. See
		// a_target_count_the_tool_takes.go.
		if err := checkTheTargetCountIsOneTheToolTakes(step, program); err != nil {
			return err
		}
		// Beside the repeat door, which judges one argv element against
		// another: a flag the flag package sets twice, keeping the last and
		// dropping a value review wrote down. See
		// a_tool_flag_the_tool_reads_once.go.
		if err := checkNoToolFlagIsNamedTwice(step, program); err != nil {
			return err
		}
		// Last on the tool branch, and the only door that reads argv ORDER:
		// for the three tools parsing with the flag package, an option
		// written after a target is not an option, because the parser has
		// already stopped. See a_tool_flag_the_parser_still_reads.go.
		if err := checkNoToolFlagFollowsATarget(step, program); err != nil {
			return err
		}
		// Last on the tool branch, and the mirror of the script companion
		// door in the exclusive direction: that one refuses an option
		// missing the companion it needs, this one an option standing
		// beside one the tool will not take. Every other door here judges
		// one element against the tool's contract; none can hold a rule
		// about a pair. See a_scan_option_the_ci_scan_allows.go.
		return checkNoToolOptionStandsBesideOneItExcludes(step, program)
	}
	if IsFrontDoorProgram(program) {
		return fmt.Errorf("%w: step %q runs %q: %w, dispatch its reviewed script instead",
			ErrInvalidCatalog, step.Name, program, ErrFrontDoorProgram)
	}
	if strings.ContainsAny(program, "/\\") {
		return fmt.Errorf("%w: step %q names the path %q as its program; dispatch a script as the first %s argument",
			ErrInvalidCatalog, step.Name, program, DispatchShell)
	}
	if program != DispatchShell {
		return fmt.Errorf("%w: step %q names %q, not %q or an %s tool",
			ErrInvalidCatalog, step.Name, program, DispatchShell, strings.TrimSuffix(ToolProgramPrefix, ":"))
	}
	if len(step.Args) == 0 || !ValidScriptPath(step.Args[0]) {
		return fmt.Errorf("%w: step %q must dispatch a reviewed script path as its first %s argument",
			ErrInvalidCatalog, step.Name, DispatchShell)
	}
	// Beside the path rule above, which judges only the SHAPE of the path:
	// this one settles whether the checkout ships that script at all. It is
	// the shell mirror of checkToolProgramExists and stands in the same
	// place, before a single argument is judged, because a script nothing
	// ships has no argument contract to judge. See
	// a_script_the_checkout_ships.go.
	if err := checkTheScriptIsOneTheCheckoutShips(step); err != nil {
		return err
	}
	// Beside the two rules above, which settle WHICH script a step
	// dispatches: this one judges what it hands that script, where the
	// script's own argument contract is stated. See
	// a_script_option_the_script_parses.go.
	if err := checkScriptOptionsAreOnesTheScriptParses(step); err != nil {
		return err
	}
	// Beside the option door, which refuses what the script cannot parse:
	// this one refuses what it parses perfectly and then returns zero
	// having done no work. See a_gate_step_runs_a_gate.go.
	if err := checkAScriptStepRunsTheWorkItNames(step); err != nil {
		return err
	}
	// Beside both doors above, which judge one argv element at a time:
	// this one refuses two the script parses individually and rejects
	// together. See a_container_step_names_a_gate.go.
	if err := checkAScriptStepNamesEveryCompanionItNeeds(step); err != nil {
		return err
	}
	// Beside the three doors above, which judge an option the script
	// runs happily: this one refuses an option the mode it selects accepts
	// and never reads, so the step passes having done something other than
	// what it names. See a_flag_the_mode_reads.go.
	if err := checkNoScriptOptionIsOneTheModeIgnores(step); err != nil {
		return err
	}
	// Beside the doors above, which read an option's name and the value
	// after it: this one judges the VALUE the script's own case arms
	// accept. See a_script_option_value_the_script_accepts.go.
	if err := checkScriptOptionValuesAreOnesTheScriptAccepts(step); err != nil {
		return err
	}
	// Last on the shell branch, and the only door here that judges one argv
	// element against another rather than against the script's contract: an
	// option the parser assigns twice, keeping the last and dropping a value
	// review wrote down. See an_option_the_script_reads_once.go.
	return checkNoScriptOptionIsNamedTwice(step)
}

// ValidateTaskDispatch applies the seam to every step of a task.
func ValidateTaskDispatch(task Task) error {
	for _, step := range task.Steps {
		if err := ValidateStepDispatch(step); err != nil {
			return fmt.Errorf("%w (task %q)", err, task.Name)
		}
	}
	if err := checkAScopeSelectorIsNamedWhereTheToolRequiresOne(task); err != nil {
		return err
	}
	// Beside the scope rule above, which judges the argv a step STATES:
	// this one judges the argv a caller may later ADD, which only the task
	// states. See a_bound_argument_the_tool_takes.go.
	if err := checkBoundArgumentsAreOnesTheToolTakes(task); err != nil {
		return err
	}
	// Beside the rule above, which asks whether the tool reads a bound
	// argument at all: this one asks whether a bound path contradicts a
	// scope the step already named. See
	// a_bound_path_beside_a_whole_tree_scan.go.
	if err := checkNoBoundPathContradictsAWholeTreeScan(task); err != nil {
		return err
	}
	// Beside the rule above, which asks whether a bound path contradicts a
	// scope the step named: this one asks whether the reviewed and bound
	// targets together fit what the tool reads. See
	// a_bound_target_the_tool_can_read.go.
	if err := checkBoundTargetsFitTheToolsCeiling(task); err != nil {
		return err
	}
	// The three rules above judge what binding adds to a TOOL step and each
	// walks past a shell one. This judges what it adds to a step
	// dispatching a script whose argument contract is stated. See
	// a_bound_argument_the_script_parses.go.
	if err := checkBoundArgumentsAreOnesTheScriptParses(task); err != nil {
		return err
	}
	// Last on the task branch: binding writes flags behind positionals and
	// appends them behind the reviewed argv, and the three flag-package
	// tools stop reading options at the first target. See
	// a_bound_flag_the_parser_still_reads.go.
	return checkNoBoundFlagLandsBehindATarget(task)
}

// validateDispatchArgs bounds what a reviewed step may put on argv. An empty
// argument is refused because no reviewed step means one and it reads as a
// value that went missing during review.
func validateDispatchArgs(step Step) error {
	for _, arg := range step.Args {
		if arg == "" || len(arg) > MaxArgumentValueBytes || !printableASCII(arg) {
			return fmt.Errorf("%w: step %q passes an unreadable argument", ErrInvalidCatalog, step.Name)
		}
	}
	return nil
}

func printableASCII(value string) bool {
	for index := 0; index < len(value); index++ {
		if value[index] < 0x20 || value[index] > 0x7e {
			return false
		}
	}
	return true
}
