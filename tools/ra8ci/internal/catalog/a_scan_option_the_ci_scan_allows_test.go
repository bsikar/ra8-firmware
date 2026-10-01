// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"errors"
	"strings"
	"testing"
)

const aloneClockProgram = ToolProgramPrefix + "runner-clock"

func aloneClockStep(args ...string) Step {
	return Step{Name: "clock", Program: aloneClockProgram, Args: args}
}

func TestACiScanStepOnItsOwnIsAdmitted(t *testing.T) {
	for _, args := range [][]string{
		{"--ci-scan"},
		{"-ci-scan"},
		{"--ci-scan=true"},
	} {
		if err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep(args...), aloneClockProgram); err != nil {
			t.Fatalf("%v is the workflow scan and nothing else: %v", args, err)
		}
	}
}

func TestAScanOptionBesideCiScanIsRefused(t *testing.T) {
	for _, args := range [][]string{
		{"--ci-scan", "--repo", "bsikar/other"},
		{"--ci-scan", "--repo=bsikar/other"},
		{"--repo", "bsikar/other", "--ci-scan"},
		{"--ci-scan", "--runs", "250"},
		{"--ci-scan", "--runs=250"},
		{"--ci-scan", "--hours", "24"},
		{"-ci-scan", "-hours=24"},
	} {
		if err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep(args...), aloneClockProgram); !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%v is a pair the tool declines: %v", args, err)
		}
	}
}

// The sharp one: --hours is read through the flag package's Visit, so the tool
// asks whether it was written rather than what it says. Its own default is
// refused exactly as any other value is, and a reviewer checking the value is
// looking at the wrong question.
func TestHoursIsRefusedBesideCiScanEvenAtItsOwnDefault(t *testing.T) {
	for _, args := range [][]string{
		{"--ci-scan", "--hours", "0"},
		{"--ci-scan", "--hours=0"},
	} {
		err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep(args...), aloneClockProgram)
		if !errors.Is(err, ErrInvalidCatalog) {
			t.Fatalf("%v is written, and written is what the tool asks: %v", args, err)
		}
		if !strings.Contains(err.Error(), "written at all") {
			t.Fatalf("the refusal must say which question the tool asks: %v", err)
		}
	}
}

// The other half of the same call: --repo and --runs are compared by VALUE,
// so naming the tool's own default asks for exactly what the tool would have
// done and is admitted. Refusing it would be the door judging tidiness rather
// than what the tool refuses.
func TestAScanOptionAtTheToolsOwnDefaultIsAdmittedBesideCiScan(t *testing.T) {
	for _, args := range [][]string{
		{"--ci-scan", "--repo", "bsikar/ra8-firmware"},
		{"--ci-scan", "--repo=bsikar/ra8-firmware"},
		{"--ci-scan", "--runs", "40"},
		{"--ci-scan", "--runs=40"},
	} {
		if err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep(args...), aloneClockProgram); err != nil {
			t.Fatalf("%v asks for the default the tool would have used: %v", args, err)
		}
	}
}

// The tool holds --runs in an int, so 040 and +40 are the number 40 there. A
// string comparison would refuse a pair the tool runs happily.
func TestANumericDefaultIsReadAsANumber(t *testing.T) {
	for _, args := range [][]string{
		{"--ci-scan", "--runs", "040"},
		{"--ci-scan", "--runs", "+40"},
		{"--ci-scan", "--runs", " 40"},
	} {
		if err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep(args...), aloneClockProgram); err != nil {
			t.Fatalf("%v is the default written another way: %v", args, err)
		}
	}
}

// A value that is not a number at all is the value door's refusal, and this
// door must not take it over with a message about pairs.
func TestAValueThatIsNotANumberIsLeftToTheValueDoor(t *testing.T) {
	if err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep("--ci-scan", "--runs", "many"), aloneClockProgram); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("a non-numeric run count is not the tool's default either: %v", err)
	}
	step := aloneClockStep("--runs", "many")
	if err := checkNoToolOptionStandsBesideOneItExcludes(step, aloneClockProgram); err != nil {
		t.Fatalf("without --ci-scan this door has no pair to judge: %v", err)
	}
}

func TestAScanOptionWithoutCiScanIsAdmitted(t *testing.T) {
	for _, args := range [][]string{
		{"--repo", "bsikar/other"},
		{"--runs", "250", "--hours", "24"},
		{"--hours=0"},
	} {
		if err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep(args...), aloneClockProgram); err != nil {
			t.Fatalf("%v is an ordinary scan: %v", args, err)
		}
	}
}

func TestTheRefusalNamesTheSpellingAndBothOptions(t *testing.T) {
	err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep("--ci-scan", "--runs", "250"), aloneClockProgram)
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{"--runs 250", "--ci-scan", "RA8_CLOCK_SCAN_RUNS"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q so a reader can find the pair and the reason: %v", want, err)
		}
	}
}

// The self test is exclusive on every reviewed tool, not on this one, so its
// refusal stays the self-test door's and keeps the better message.
func TestTheSelfTestKeepsItsOwnDoorsRefusal(t *testing.T) {
	step := aloneClockStep("--selftest", "--runs", "250")
	if err := checkNoToolOptionStandsBesideOneItExcludes(step, aloneClockProgram); err != nil {
		t.Fatalf("this door states no rule about --selftest: %v", err)
	}
	if err := checkASelftestStepNamesNothingElse(step, aloneClockProgram); !errors.Is(err, ErrInvalidCatalog) {
		t.Fatalf("the self-test door must still make this refusal: %v", err)
	}
}

// The flag package stops reading options at the first element that is not one,
// so nothing past a target is a flag this door can see. The order door owns
// what that costs.
func TestNothingPastATargetIsReadAsAFlag(t *testing.T) {
	if err := checkNoToolOptionStandsBesideOneItExcludes(aloneClockStep("--ci-scan", "some/path", "--hours", "24"), aloneClockProgram); err != nil {
		t.Fatalf("--hours here is a positional the parser never reads: %v", err)
	}
}

func TestAToolStatingNoExclusionsIsAdmitted(t *testing.T) {
	step := Step{Name: "scan", Program: ToolProgramPrefix + "ascii", Args: []string{"--all", "--check"}}
	if err := checkNoToolOptionStandsBesideOneItExcludes(step, ToolProgramPrefix+"ascii"); err != nil {
		t.Fatalf("a tool absent from the table is admitted on its flags alone: %v", err)
	}
}

// The table restates a tool contract, so it must not drift from the flags the
// tool is reviewed as parsing.
func TestEveryStatedOptionIsOneTheToolParses(t *testing.T) {
	for program, rules := range toolOptionsStandingAlone {
		accepted, known := ReviewedToolFlags(program)
		if !known {
			t.Fatalf("%q states exclusions and no reviewed flags", program)
		}
		for _, rule := range rules {
			if !statesFlag(accepted, rule.option) {
				t.Fatalf("%q states an exclusive option %q the tool does not parse", program, rule.option)
			}
			for _, excluded := range rule.excludes {
				if !statesFlag(accepted, excluded.name) {
					t.Fatalf("%q excludes %q, which the tool does not parse", rule.option, excluded.name)
				}
				if excluded.onPresence {
					continue
				}
				if !ToolFlagTakesAValue(program, excluded.name) {
					t.Fatalf("%q is judged by value and carries none", excluded.name)
				}
			}
		}
	}
}

func TestToolOptionStandsAloneAnswersForTheStatedOption(t *testing.T) {
	excluded, stated := ToolOptionStandsAlone(aloneClockProgram, "ci-scan")
	if !stated || len(excluded) != 3 {
		t.Fatalf("want the three scan options, got %v (%t)", excluded, stated)
	}
	if _, stated := ToolOptionStandsAlone(aloneClockProgram, "runs"); stated {
		t.Fatal("--runs composes; only --ci-scan stands alone")
	}
	if _, stated := ToolOptionStandsAlone(ToolProgramPrefix+"ascii", "all"); stated {
		t.Fatal("ascii states no exclusions")
	}
}

func TestTheShippedCatalogNamesNoExcludedPair(t *testing.T) {
	if _, err := Load(); err != nil {
		t.Fatalf("the embedded catalog must pass this door: %v", err)
	}
}
