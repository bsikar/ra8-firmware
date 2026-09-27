// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strconv"
	"strings"
)

// exclusiveToolOption states one option a reviewed tool refuses to run beside
// another, and which options it refuses.
type exclusiveToolOption struct {
	// option is the flag that must stand alone, without dashes.
	option string
	// excludes are the flags it refuses to stand beside.
	excludes []excludedToolOption
	// because is the tool's own reason, for a refusal that teaches.
	because string
}

// excludedToolOption states one flag an exclusive option refuses, and HOW the
// tool decides it was named. Those are two different questions and this tool
// answers them differently for different flags, which is most of why the door
// is worth having: a reader cannot tell which rule applies to which flag
// without reading the parser.
type excludedToolOption struct {
	// name is the flag, without dashes.
	name string
	// onPresence is set where the tool asks whether the flag was written at
	// all, through the flag package's Visit, so the flag's own default value
	// does not excuse writing it.
	onPresence bool
	// theToolsDefault is the value the tool compares the flag against where it
	// judges by value rather than presence. Read only when onPresence is unset.
	theToolsDefault string
	// numeric is set where the tool holds the value in an int, so that 40, 040
	// and +40 are one value and only a string comparison would disagree.
	numeric bool
}

// toolOptionsStandingAlone states, per reviewed tool, the options that cannot
// be dispatched beside another.
//
// runner-clock is the only tool here and the only one that needs to be: it is
// the one dispatched tool that reaches the network, and the only one with
// flags carrying values at all. Its contract is restated rather than imported,
// the same call reviewedToolFlags makes for eighteen tools, because the review
// layer states what it admits rather than depending on the implementation it
// judges.
//
// Read out of runnerclock.go's Run: --ci-scan refuses --repo and --runs when
// their values differ from the tool's own defaults, and refuses --hours on
// presence alone, through flags.Visit. The --selftest half of the same guard
// is not stated here; the self-test door owns it and its message is the better
// one, because the self test is exclusive on every reviewed tool rather than
// on this one.
var toolOptionsStandingAlone = map[string][]exclusiveToolOption{
	ToolProgramPrefix + "runner-clock": {{
		option:  "ci-scan",
		because: "--ci-scan takes its run count from RA8_CLOCK_SCAN_RUNS so a workflow can move it without editing a reviewed task, and a scan option written beside it is the definition and the environment asking for different numbers",
		excludes: []excludedToolOption{
			{name: "repo", theToolsDefault: "bsikar/ra8-firmware"},
			{name: "runs", theToolsDefault: "40", numeric: true},
			{name: "hours", onPresence: true},
		},
	}},
}

// ToolOptionStandsAlone returns the flags a reviewed tool refuses to run beside
// the named option, and is how a caller can tell an option that must be
// dispatched on its own from one that composes.
func ToolOptionStandsAlone(program, option string) ([]string, bool) {
	for _, rule := range toolOptionsStandingAlone[program] {
		if rule.option != option {
			continue
		}
		excluded := make([]string, 0, len(rule.excludes))
		for _, item := range rule.excludes {
			excluded = append(excluded, "--"+item.name)
		}
		return excluded, true
	}
	return nil, false
}

// checkNoToolOptionStandsBesideOneItExcludes refuses a reviewed step that hands
// a tool two options it parses happily on their own and declines together.
//
// It is the tool mirror of the script companion door, in the exclusive
// direction: that one refuses an option missing the companion it needs, this
// one refuses an option standing beside a companion it will not take. Every
// other door on the tool branch judges one argv element against the tool's
// contract, or one element against another of the same kind; none can hold a
// rule about a PAIR the tool itself rejects.
//
// Nothing in the argv is malformed. --ci-scan is a flag the tool parses,
// --hours is a flag the tool parses, and the step reads as though it asks for
// a workflow scan over a window. The tool prints that the two cannot be
// combined and exits 2 having scanned nothing, on a runner, in a guest, long
// after review admitted the step.
//
// The sharpest entry is --hours, because it is judged on presence rather than
// value: --ci-scan --hours=0 names the flag's own default, changes nothing a
// reader can see, and is refused exactly as --hours=24 is. A reviewer checking
// the value finds nothing wrong with it, and is looking at the wrong question.
//
// An admission rule, applied where a manifest is read rather than against a
// task already persisted under a reviewed digest, the same line every door on
// this seam draws.
func checkNoToolOptionStandsBesideOneItExcludes(step Step, program string) error {
	rules, stated := toolOptionsStandingAlone[program]
	if !stated {
		return nil
	}
	written := toolFlagsAsWritten(step.Args, program)
	if len(written) == 0 {
		return nil
	}
	for _, rule := range rules {
		if _, standing := written[rule.option]; !standing {
			continue
		}
		for _, excluded := range rule.excludes {
			beside, present := written[excluded.name]
			if !present {
				continue
			}
			if !excluded.onPresence && sameAsTheToolsDefault(beside.value, excluded) {
				continue
			}
			return fmt.Errorf("%w: step %q passes %s to %q beside --%s, which the tool declines as a pair and answers with a usage error and no scan at all; %s%s",
				ErrInvalidCatalog, step.Name, beside.spelling, program, rule.option, rule.because, howItIsJudged(excluded))
		}
	}
	return nil
}

// howItIsJudged names the half of the rule a reader cannot see from the argv,
// so the refusal teaches rather than just declining.
func howItIsJudged(excluded excludedToolOption) string {
	if excluded.onPresence {
		return fmt.Sprintf(". --%s is judged by whether it was written at all rather than by its value, so even its own default is refused here", excluded.name)
	}
	return fmt.Sprintf(". --%s is admitted beside it only at the tool's own default of %q", excluded.name, excluded.theToolsDefault)
}

// toolArgumentWritten is one flag as the step spelled it, kept so a refusal can
// quote the elements a reader would go looking for.
type toolArgumentWritten struct {
	spelling string
	value    string
}

// toolFlagsAsWritten reads a step's arguments the way the flag package does:
// options until the first element that is not one, with a value-carrying flag
// swallowing the element after it. A flag named twice keeps the last, which is
// what the parser does with it; the repeat door has already refused the pair by
// the time this one runs.
func toolFlagsAsWritten(args []string, program string) map[string]toolArgumentWritten {
	written := make(map[string]toolArgumentWritten, len(args))
	for index := 0; index < len(args); index++ {
		arg := args[index]
		if !strings.HasPrefix(arg, "-") || arg == endOfOptions {
			break
		}
		name := flagNameArgvClaims(arg)
		if name == "" {
			break
		}
		if _, after, split := strings.Cut(arg, "="); split {
			written[name] = toolArgumentWritten{spelling: arg, value: after}
			continue
		}
		if toolFlagTakesAValue(program, name) && index+1 < len(args) {
			written[name] = toolArgumentWritten{spelling: arg + " " + args[index+1], value: args[index+1]}
			index++
			continue
		}
		written[name] = toolArgumentWritten{spelling: arg}
	}
	return written
}

// sameAsTheToolsDefault reports whether a written value is the one the tool
// would have used anyway. A numeric flag is compared as a number, because the
// tool holds it in an int and 040 is 40 there; a value that is not a number at
// all is the value door's refusal, not this one's, so it falls back to the
// text and lets that door make it.
func sameAsTheToolsDefault(value string, excluded excludedToolOption) bool {
	if excluded.numeric {
		mine, mineErr := strconv.Atoi(strings.TrimSpace(value))
		theirs, theirsErr := strconv.Atoi(excluded.theToolsDefault)
		if mineErr == nil && theirsErr == nil {
			return mine == theirs
		}
	}
	return value == excluded.theToolsDefault
}
