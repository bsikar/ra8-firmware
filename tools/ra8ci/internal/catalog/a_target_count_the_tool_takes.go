// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"sort"
	"strings"
)

// The last question the reviewed argv of a tool step asks, and the one the
// file-argument door leaves open.
//
// a_file_argument_the_tool_reads.go splits the eighteen dispatched tools in
// two: eleven read no file argument at all and refuse any, seven take a path
// and resolve it against the checkout they were handed. For six of those
// seven, HOW MANY paths is genuinely the checkout's question: assert-casts,
// final-newline, gnu-attribute, no-null, since and tz-boundary-discard each
// scan every target on argv, so a step naming one file and a step naming forty
// are the same shape of dispatch and review has nothing to say about the
// count.
//
// ascii is not built that way and is the only one that is not. parseOptions
// (asciigate.go) answers scope with an exclusive choice: either --all and no
// target, or EXACTLY ONE target, optionally with --checkout. The test is
// `(!*all && flags.NArg() != 1)` and it errors with "pass one target path,
// optionally with --checkout, or pass --all", exit 2, BEFORE the tree is
// touched. Run then reads opts.target, a single string, and hands it to
// checkoutTargets or walkTargets: there is nowhere for a second path to go.
//
// So `ra8ci:ascii src/ra8_batt.c src/ra8_ui.c` was admitted by review,
// digested into the catalog, dispatched to a guest and answered with a usage
// line on every runner, forever. The tool name door admitted it (the tool
// exists), the option door admitted it (no argument claims to be an option),
// and the file door admitted it (ascii does read a path). Only the COUNT is
// wrong, and nothing was reading the count.
//
// THE CEILING IS PER TOOL, NOT A GENERAL ARITY RULE. Writing one would refuse
// the six that scan a list, which is the mirror of the mistake the scope-
// selector door (a_scope_selector_the_tool_requires.go) had to back out of: a
// uniform rule over a seam whose tools deliberately differ refuses work the
// tools would do. The table below therefore states a ceiling only where the
// tool states one, and TestEveryToolThatReadsFilesSaysHowManyItTakes holds it
// to toolsReadingFileArguments in both directions, so a tool cannot gain a
// ceiling in its own parser and keep an unbounded one here.
//
// SAYS NOTHING ABOUT --all BESIDE A PATH: that pair is a contradiction about
// scope rather than a count, and an_all_step_names_the_whole_tree.go refuses
// it one door earlier. A step naming --all reaches this rule with no target to
// count and passes, which is correct: the refusal it deserves already
// happened, with the message that names the real mistake.
//
// Restated, not imported, like every table on this seam: the executor imports
// the catalog, so the catalog cannot ask a tool what its parser accepts.
var toolsTakingOneTarget = map[string]int{
	"ra8ci:ascii": 1, // parseOptions: (!*all && flags.NArg() != 1) is a usage error
}

// ToolTargetCeiling reports how many file arguments a reviewed ra8ci: tool
// takes, and whether it states a ceiling at all. A tool that reads file
// arguments without a ceiling scans every target on its argv.
func ToolTargetCeiling(program string) (int, bool) {
	ceiling, stated := toolsTakingOneTarget[program]
	return ceiling, stated
}

// checkTheTargetCountIsOneTheToolTakes refuses a reviewed step naming more
// file arguments than the dispatched tool can read. It is an admission rule,
// applied where a manifest is read rather than against a task already
// persisted under a reviewed digest, the same line the other argv doors draw.
func checkTheTargetCountIsOneTheToolTakes(step Step, program string) error {
	ceiling, stated := toolsTakingOneTarget[program]
	if !stated {
		return nil
	}
	targets := targetsNamed(step.Args, valueTakingToolFlags[program])
	if len(targets) <= ceiling {
		return nil
	}
	return fmt.Errorf("%w: step %q passes %d file arguments to %q, which takes %s; it would answer %q and %q with a usage error before reading anything; pass one target, or --all to scan the whole tree",
		ErrInvalidCatalog, step.Name, len(targets), program, targetWord(ceiling),
		targets[0], targets[1])
}

// targetsNamed returns the argv elements a tool would read as file arguments,
// skipping options and the separate value of an option that takes one. It
// reads a dash-led element the same way every other door on this seam does,
// through flagNameArgvClaims, so -checkout and --checkout=1 are options here
// exactly as they are there.
func targetsNamed(args []string, valued map[string]bool) []string {
	targets := make([]string, 0, len(args))
	expectingValue := false
	for _, arg := range args {
		if expectingValue {
			expectingValue = false
			continue
		}
		if strings.HasPrefix(arg, "-") {
			expectingValue = valued[flagNameArgvClaims(arg)] && !strings.Contains(arg, "=")
			continue
		}
		targets = append(targets, arg)
	}
	return targets
}

func targetWord(ceiling int) string {
	if ceiling == 1 {
		return "exactly one"
	}
	return fmt.Sprintf("at most %d", ceiling)
}

// toolsStatingATargetCeiling names, sorted, the tools that bound how many file
// arguments they read. Reported so a refusal elsewhere can say which tools are
// counted rather than only that this one is.
func toolsStatingATargetCeiling() []string {
	named := make([]string, 0, len(toolsTakingOneTarget))
	for program := range toolsTakingOneTarget {
		named = append(named, program)
	}
	sort.Strings(named)
	return named
}
