// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"sort"
	"strings"
)

// The other half of a reviewed tool step's argv, and the companion of
// a_tool_flag_the_tool_parses.go.
//
// That door reads every argument that CLAIMS to be an option and holds it to
// the flags the tool's own parser states. It deliberately says nothing about
// the rest, because an argument without a leading dash reads as a file the
// tool resolves against the checkout it was handed, and whether a path exists
// there is the checkout's question rather than review's.
//
// That reasoning holds for seven of the eighteen dispatched tools. It is
// wrong for the other eleven, and the difference is not a matter of degree:
// they take NO file arguments at all, so a path handed to one is not a target
// that may or may not exist, it is an argument the tool has no reading for.
// Each of the eleven says so in its own way and every one of them stops before
// reading the tree: the nine that compare argv exactly answer anything past
// --selftest with `if len(args) != 0` and a usage line, and the two flag-package
// tools (runner-clock, tests-readme) require flags.NArg() == 0 and print usage
// otherwise. All exit 2.
//
// So a reviewed step dispatching ra8ci:legacy-make src/ra8_batt.c, or
// ra8ci:tests-readme tests/, was admitted by review, digested into the
// catalog, dispatched to a guest, and came back exit 2 with a usage line,
// once per attempt, on every runner, forever. It is the same shape of failure
// the option door closes, reached by the one kind of argument that door lets
// through.
//
// wave-references is again the exception that argues FOR the rule rather than
// against it. waverefs.Run derives its own scope from the checkout and never
// looks at argv except to find --selftest, so a path passed to it is SILENTLY
// IGNORED: the step runs, scans the whole tree, and files a green or red gate
// result for a scope nobody asked for, while the reviewed manifest reads as
// though that step judged one file. A wrong verdict is worse than a usage
// error, which is why it belongs with the eleven and not with the seven.
//
// THE SET IS RESTATED, NOT IMPORTED, for the same reason the tool names and
// their flags are: the executor imports the catalog, so the catalog cannot ask
// the executor what its tools read. Each entry below is that tool's own argv
// handling in internal/<tool>.Run, and
// TestEveryReviewedToolSaysWhetherItReadsFileArguments holds the set to the
// dispatch list, so a tool cannot be added in one place and forgotten here.
var toolsReadingFileArguments = map[string]bool{
	"ra8ci:ascii":               true, // parseOptions takes one target path unless --all
	"ra8ci:assert-casts":        true, // scans the files on argv, or --all
	"ra8ci:final-newline":       true, // explicitTargets(root, args)
	"ra8ci:gnu-attribute":       true, // files := args, discovering only when argv is empty
	"ra8ci:no-null":             true, // candidates come from argv unless --all
	"ra8ci:since":               true, // paths come from argv unless --all
	"ra8ci:tz-boundary-discard": true, // files := args, discovering only when argv is empty
}

// ToolReadsFileArguments reports whether a reviewed ra8ci: tool reads file
// arguments off its argv at all.
func ToolReadsFileArguments(program string) bool {
	return toolsReadingFileArguments[program]
}

// checkFileArgumentsAreOnesTheToolReads refuses a reviewed step that hands a
// file argument to a tool that reads none. It is an admission rule, applied
// where a manifest is read rather than against a task already persisted under
// a reviewed digest, the same line checkToolProgramExists and
// checkToolFlagsAreOnesTheToolParses draw.
func checkFileArgumentsAreOnesTheToolReads(step Step, program string) error {
	if toolsReadingFileArguments[program] {
		return nil
	}
	valued := valueTakingToolFlags[program]
	expectingValue := false
	for _, arg := range step.Args {
		if expectingValue {
			expectingValue = false
			continue
		}
		if strings.HasPrefix(arg, "-") {
			name := flagNameArgvClaims(arg)
			expectingValue = valued[name] && !strings.Contains(arg, "=")
			continue
		}
		return fmt.Errorf("%w: step %q passes %q to %q, which reads no file arguments and answers argv past its options with a usage error; only %s take a path",
			ErrInvalidCatalog, step.Name, arg, program, strings.Join(toolsThatReadFileArguments(), ", "))
	}
	return nil
}

// valueTakingToolFlags names, per tool, the options whose value is a SEPARATE
// argv element, so this door does not read that value as a file argument. The
// flag package accepts both --repo=X and --repo X, and only the second form
// puts a bare word on argv. Today runner-clock is the only dispatched tool
// with a non-boolean flag; every other reviewed flag is a mode switch, which
// is why the map has one entry rather than eighteen.
// TestEveryValueTakingFlagIsOneItsToolParses holds these names to the option
// table, so a renamed flag cannot leave a value quietly readable as a path.
var valueTakingToolFlags = map[string]map[string]bool{
	"ra8ci:runner-clock": {"repo": true, "runs": true, "hours": true},
}

// toolsThatReadFileArguments names the tools a path may be handed to, so the
// refusal points at the set rather than only at the one tool that was wrong.
func toolsThatReadFileArguments() []string {
	named := make([]string, 0, len(toolsReadingFileArguments))
	for program := range toolsReadingFileArguments {
		named = append(named, program)
	}
	sort.Strings(named)
	return named
}
