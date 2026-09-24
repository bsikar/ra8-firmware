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
var frontDoorPrograms = []string{"just", "make", "gmake", "cmake", "ninja"}

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
		return nil
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
	return nil
}

// ValidateTaskDispatch applies the seam to every step of a task.
func ValidateTaskDispatch(task Task) error {
	for _, step := range task.Steps {
		if err := ValidateStepDispatch(step); err != nil {
			return fmt.Errorf("%w (task %q)", err, task.Name)
		}
	}
	return nil
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
