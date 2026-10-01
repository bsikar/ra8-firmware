// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"fmt"
	"strconv"
	"strings"
)

// The one axis the option door leaves open.
//
// a_tool_flag_the_tool_parses.go holds which option NAMES a reviewed step may
// hand a tool, read out of each tool's own parser. It says nothing about the
// VALUES, and for seventeen of the eighteen tools that is the whole story,
// because every flag they parse is a mode switch that carries no value.
//
// runner-clock is the exception, and it is also the only dispatched tool that
// reaches the network, which is what makes the gap cost a real run rather than
// a usage line. Three of its flags take a value, and its own Run refuses a bad
// one AFTER flag.Parse has accepted it:
//
//	--runs N   "runner-clock: --runs must be at least 1; a scan of nothing
//	           proves nothing." Exit 2.
//	--hours N  "runner-clock: --hours cannot be negative". Exit 2.
//	--repo R   "runner-clock: --repo must be owner/repository using GitHub
//	           name characters". Exit 2.
//
// A value the flag package cannot parse at all fails one step earlier and is
// no better: flags.Parse returns an error and Run prints its usage line, also
// exit 2. Either way a reviewed step passing --runs 0, --hours -1 or
// --repo "not a repo" was admitted by review, digested into the catalog,
// dispatched to a guest, and answered the same way on every runner, forever.
// That is the shape every door on this seam closes, reached through the one
// argument kind the option door deliberately skips: an option's value.
//
// THE BOUNDS ARE RESTATED, NOT IMPORTED, for the reason the tool names, their
// flags, their file-reading and their scope all are: the executor imports the
// catalog, so the catalog cannot ask a tool what it accepts. Each rule below
// is runner-clock's own check, and the repo shape is its repoPattern read as
// the two conditions that pattern states: two slash-separated segments of
// GitHub name characters, each 1 to 100 bytes, neither "." nor "..".
//
// DELIBERATELY NOT A RANGE CEILING on --runs or --hours. runner-clock states a
// floor for each and no ceiling, and inventing one here would refuse a scan
// the tool would happily run. The rule only refuses what the tool itself
// refuses.
var toolFlagValueRules = map[string]map[string]func(string) error{
	"ra8ci:runner-clock": {
		"runs":  atLeast(1, "--runs must be at least 1; a scan of nothing proves nothing"),
		"hours": atLeast(0, "--hours cannot be negative"),
		"repo":  checkRepoValue,
	},
}

// atLeast builds the value rule for an integer flag with a floor and no
// ceiling, which is the shape both of runner-clock's numeric flags have.
func atLeast(floor int, because string) func(string) error {
	return func(value string) error {
		parsed, err := strconv.Atoi(value)
		if err != nil {
			return fmt.Errorf("%q is not an integer", value)
		}
		if parsed < floor {
			return fmt.Errorf("%s", because)
		}
		return nil
	}
}

// checkRepoValue restates runner-clock's repoPattern plus the dot checks
// beside it.
func checkRepoValue(value string) error {
	segments := strings.Split(value, "/")
	if len(segments) != 2 {
		return fmt.Errorf("%q is not owner/repository", value)
	}
	for _, segment := range segments {
		if len(segment) == 0 || len(segment) > maxRepoSegmentBytes {
			return fmt.Errorf("%q is not owner/repository", value)
		}
		if segment == "." || segment == ".." {
			return fmt.Errorf("%q names a directory, not a repository", value)
		}
		for _, letter := range segment {
			if !isGitHubNameByte(letter) {
				return fmt.Errorf("%q carries %q, which is not a GitHub name character", value, letter)
			}
		}
	}
	return nil
}

// maxRepoSegmentBytes is the bound runner-clock's repoPattern states for each
// side of the slash.
const maxRepoSegmentBytes = 100

func isGitHubNameByte(letter rune) bool {
	switch {
	case letter >= 'A' && letter <= 'Z', letter >= 'a' && letter <= 'z', letter >= '0' && letter <= '9':
		return true
	case letter == '_', letter == '.', letter == '-':
		return true
	}
	return false
}

// checkToolFlagValuesAreOnesTheToolAccepts refuses a reviewed step handing a
// tool an option value that tool refuses. It is an admission rule, applied
// where a manifest is read rather than against a task already persisted under
// a reviewed digest, the same line the other doors on this seam draw.
//
// A value supplied by a BOUND argument is not judged here and cannot be: it
// arrives from the caller at dispatch, long after review, and
// checkBoundArgumentsKeepTheirMeaning already refuses a reviewed argument that
// collides with a declared flag name, so the two cannot be the same flag.
func checkToolFlagValuesAreOnesTheToolAccepts(step Step, program string) error {
	rules := toolFlagValueRules[program]
	if len(rules) == 0 {
		return nil
	}
	for index := 0; index < len(step.Args); index++ {
		arg := step.Args[index]
		if !strings.HasPrefix(arg, "-") {
			continue
		}
		name := flagNameArgvClaims(arg)
		rule, judged := rules[name]
		if !judged {
			continue
		}
		value, found := "", false
		if inline := strings.IndexByte(arg, '='); inline >= 0 {
			value, found = arg[inline+1:], true
		} else if index+1 < len(step.Args) {
			index++
			value, found = step.Args[index], true
		}
		if !found {
			return fmt.Errorf("%w: step %q passes %q to %q with no value; the flag takes one and the tool answers a bare flag with its usage line",
				ErrInvalidCatalog, step.Name, arg, program)
		}
		if err := rule(value); err != nil {
			return fmt.Errorf("%w: step %q passes --%s=%s to %q, which refuses it: %s",
				ErrInvalidCatalog, step.Name, name, value, program, err)
		}
	}
	return nil
}
