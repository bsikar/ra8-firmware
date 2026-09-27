// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import "fmt"

// The self-test mode of an ra8ci: tool is exclusive, and every tool says so in
// its own way.
//
// a_tool_flag_the_tool_parses.go closed the option NAMES a step may hand a
// tool. It says nothing about combinations, and there is exactly one
// combination every dispatched tool refuses: --selftest beside anything else.
// The flag-package tools say it outright (asciigate.parseOptions returns
// "--selftest cannot be combined with other modes"; runnerclock returns
// "--selftest cannot be combined with scan options"; testsreadme parses
// selftest and then requires NArg() == 0). The fifteen tools that compare argv
// exactly say it by shape, reading the self test only as len(args) == 1 &&
// args[0] == "--selftest" and answering anything longer with usage. All of
// them exit 2.
//
// So a reviewed step dispatching ra8ci:ascii --selftest --all, or
// ra8ci:no-null --selftest src/ra8_batt.c, was admitted by review and answered
// with a usage line on every runner, once per attempt, forever. It is the same
// failure the option door closes, reached by two names each of which is
// separately fine.
//
// wave-references is again the one that does not exit 2: waverefs.Run looks
// for --selftest anywhere in argv and runs the self test whatever else is
// there. That is worse rather than better, because the step passes, and what
// it proves is that the detector works, not that the tree is clean. A reviewed
// task that means to scan and accidentally also says --selftest files a green
// gate result nobody read the repository for.
//
// The reviewed manifest already pairs the two honestly: every tool task opens
// with a step whose whole argv is --selftest and then runs its scan as a
// separate step. This rule states that pairing rather than leaving it to
// convention.
func checkASelftestStepNamesNothingElse(step Step, program string) error {
	if len(step.Args) < 2 {
		return nil
	}
	for _, arg := range step.Args {
		if flagNameArgvClaims(arg) != selftestFlag {
			continue
		}
		return fmt.Errorf("%w: step %q asks %q for its self test and passes %d argument(s) beside it; the self test is exclusive on every reviewed tool, so dispatch the scan as its own step",
			ErrInvalidCatalog, step.Name, program, len(step.Args)-1)
	}
	return nil
}

// selftestFlag is the option every reviewed tool reads as "prove the detector,
// then exit" rather than "read the tree".
const selftestFlag = "selftest"
