// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"errors"
	"io/fs"
	"path/filepath"
	"regexp/syntax"
	"strings"
	"testing"
)

const contradictionManifest = "examples/ek_ra8d2/hw_validated/hil/uart_expect/hil.conf"

// manifestAsserting builds the smallest manifest carrying both halves of the
// text-capture assertion, so a test varies one line and nothing else.
func manifestAsserting(mode Mode, expect, negative string) string {
	body := "HIL_MODE=" + string(mode) + "\n"
	if expect != "" {
		body += "HIL_EXPECT=\"" + expect + "\"\n"
	}
	if negative != "" {
		body += "HIL_EXPECT_NEGATIVE=\"" + negative + "\"\n"
	}
	return body
}

func parseAsserting(t *testing.T, mode Mode, expect, negative string) (Spec, error) {
	t.Helper()
	return Parse(strings.NewReader(manifestAsserting(mode, expect, negative)), contradictionManifest)
}

func assertingSpec(mode Mode, expect, negative string) Spec {
	return Spec{Path: contradictionManifest, Mode: mode, Expect: expect,
		ExpectNegative: negative, Values: map[string]Value{}}
}

func TestAnExpectationTheManifestAlsoForbidsIsRefusedBeforeTheBoard(t *testing.T) {
	for _, mode := range []Mode{ModeUARTScrape, ModeRTTScrape} {
		spec, err := parseAsserting(t, mode, "verdict=FAILSAFE ok=Y", "FAIL|HardFault")
		if !errors.Is(err, ErrInvalidManifest) {
			t.Fatalf("%s: accepted a manifest that forbids its own expectation: %v (%+v)", mode, err, spec)
		}
		if !strings.Contains(err.Error(), "FAIL") || !strings.Contains(err.Error(), contradictionManifest) {
			t.Errorf("%s: refusal names neither the forbidden text nor the manifest: %v", mode, err)
		}
	}
}

func TestTheRefusedManifestCouldNeverHavePassed(t *testing.T) {
	spec := assertingSpec(ModeRTTScrape, "verdict=FAILSAFE ok=Y", "FAIL|HardFault")
	if err := checkExpectationIsNotAlsoForbidden(spec); err == nil {
		t.Fatal("rule accepted the contradiction under test")
	}
	// The claim the rule rests on: any capture that satisfies the positive
	// expectation carries the forbidden text by that very fact.
	for _, capture := range []string{
		"verdict=FAILSAFE ok=Y\n",
		"boot ok\nverdict=FAILSAFE ok=Y\ndone\n",
		"noise verdict=FAILSAFE ok=Y trailing",
	} {
		if err := VerifyTextCapture(spec, []byte(capture)); !errors.Is(err, ErrNegativeExpectation) {
			t.Errorf("capture %q reached %v, not the guaranteed negative match", capture, err)
		}
	}
}

func TestTheRepositoryShapeOfAssertionIsAccepted(t *testing.T) {
	for _, negative := range []string{
		"FAIL|HardFault|TIMEOUT",
		"HardFault|hw_init_failed|mismatch at|TIMEOUT|FAILED",
		"HardFault|hw_init_failed|lpm.*failed|cgc_init failed|rtc.*failed",
		"rot verify: FAIL|(bug)|HardFault",
	} {
		if _, err := parseAsserting(t, ModeUARTScrape, "verdict=PASS ok=Y", negative); err != nil {
			t.Errorf("refused a manifest whose expectation is clean of %q: %v", negative, err)
		}
	}
}

func TestUARTFoldsCaseTheWayItsVerdictDoes(t *testing.T) {
	if _, err := parseAsserting(t, ModeUARTScrape, "verdict=fail is fine", "FAIL"); !errors.Is(err, ErrInvalidManifest) {
		t.Error("uart_scrape greps with -i, so a folded forbidden text still contradicts")
	}
	if _, err := parseAsserting(t, ModeRTTScrape, "verdict=fail is fine", "FAIL"); err != nil {
		t.Errorf("rtt_scrape does not fold case, so this manifest can still pass: %v", err)
	}
}

func TestOnlyBranchesThatFireOnTheirOwnAreJudged(t *testing.T) {
	for _, negative := range []string{
		"^verdict=PASS",  // an anchor the expectation cannot promise
		"verdict=P.SS",   // a class the expectation need not supply
		"verdict=PASSS*", // a quantifier over text beyond the expectation
	} {
		if _, err := parseAsserting(t, ModeUARTScrape, "verdict=PASS ok=Y", negative); err != nil {
			t.Errorf("%q is conditional on the capture, so only the run can judge it: %v", negative, err)
		}
	}
	// The end anchor says the same thing at the other end of the line. The
	// grammar refuses a "$" in any manifest value, so the rule is asked
	// directly rather than through a manifest that cannot be written.
	if err := checkExpectationIsNotAlsoForbidden(assertingSpec(ModeUARTScrape, "verdict=PASS ok=Y", "verdict=PASS$")); err != nil {
		t.Errorf("an end anchor is conditional on the capture too: %v", err)
	}
}

func TestAPlainForbiddenPatternIsJudgedWhole(t *testing.T) {
	if _, err := parseAsserting(t, ModeRTTScrape, "boot verdict=PASS", "verdict=PASS"); !errors.Is(err, ErrInvalidManifest) {
		t.Error("a literal negative expectation inside the positive one is the same contradiction")
	}
}

func TestACapturingGroupIsItsContents(t *testing.T) {
	if _, err := parseAsserting(t, ModeRTTScrape, "state=(bug) cleared", "HardFault|(bug)"); !errors.Is(err, ErrInvalidManifest) {
		t.Error("a captured literal forbids exactly its contents")
	}
}

func TestAnEmptyBranchForbidsEveryCapture(t *testing.T) {
	spec := assertingSpec(ModeUARTScrape, "verdict=PASS ok=Y", "HardFault|")
	if err := checkExpectationIsNotAlsoForbidden(spec); !errors.Is(err, ErrInvalidManifest) {
		t.Errorf("an empty alternative matches every capture ever taken: %v", err)
	}
	if err := VerifyTextCapture(spec, []byte("verdict=PASS ok=Y\n")); !errors.Is(err, ErrNegativeExpectation) {
		t.Errorf("the verdict agrees it can never pass, reached %v", err)
	}
}

func TestAManifestWithOnlyOneHalfOfTheAssertionIsLeftAlone(t *testing.T) {
	for _, spec := range []Spec{
		assertingSpec(ModeUARTScrape, "verdict=PASS ok=Y", ""),
		assertingSpec(ModeUARTScrape, "", "verdict=PASS ok=Y"),
	} {
		if err := checkExpectationIsNotAlsoForbidden(spec); err != nil {
			t.Errorf("%+v: nothing contradicts anything here: %v", spec, err)
		}
	}
}

func TestModesThatNeverReadTheAssertionAreNotJudged(t *testing.T) {
	for _, mode := range []Mode{ModeAlive, ModeJLinkMemprobe, ModeEthernetTCP, ModeC6CameraLivestream} {
		spec := assertingSpec(mode, "verdict=FAIL", "FAIL")
		if err := checkExpectationIsNotAlsoForbidden(spec); err != nil {
			t.Errorf("%s reaches its verdict by other means: %v", mode, err)
		}
		if err := VerifyTextCapture(spec, []byte("verdict=FAIL\n")); !errors.Is(err, ErrUnsupportedCaptureMode) {
			t.Errorf("%s should never reach a text-capture verdict, reached %v", mode, err)
		}
	}
}

func TestAPatternThisBuildCannotApplyStaysTheOtherRulesRefusal(t *testing.T) {
	spec := assertingSpec(ModeUARTScrape, "verdict=PASS ok=Y", "HardFault[")
	if err := checkExpectationIsNotAlsoForbidden(spec); err != nil {
		t.Errorf("a malformed pattern is a different defect: %v", err)
	}
	if _, err := parseAsserting(t, ModeUARTScrape, "verdict=PASS ok=Y", "HardFault["); !errors.Is(err, ErrInvalidManifest) {
		t.Error("and Parse still refuses it, through the rule that owns it")
	}
}

func TestTheForbiddenTextsOfAPattern(t *testing.T) {
	for _, tc := range []struct {
		pattern string
		texts   []string
	}{
		{"FAIL", []string{"FAIL"}},
		{"FAIL|HardFault", []string{"FAIL", "HardFault"}},
		{"FAIL|lpm.*failed|TIMEOUT", []string{"FAIL", "TIMEOUT"}},
		{"^HardFault", nil},
		{"(bug)", []string{"bug"}},
		{"a.*b", nil},
	} {
		parsed := mustParsePattern(t, tc.pattern)
		var texts []string
		for _, literal := range unconditionalLiterals(parsed) {
			texts = append(texts, literal.text)
		}
		if strings.Join(texts, "\x00") != strings.Join(tc.texts, "\x00") {
			t.Errorf("%q forbids %q on its own, read as %q", tc.pattern, tc.texts, texts)
		}
	}
}

// mustParsePattern reads a negative expectation the way the rule reads one.
func mustParsePattern(t *testing.T, pattern string) *syntax.Regexp {
	t.Helper()
	parsed, err := syntax.Parse(pattern, syntax.Perl)
	if err != nil {
		t.Fatalf("%q: %v", pattern, err)
	}
	return parsed
}

func TestEveryRepositoryManifestHoldsItsOwnAssertion(t *testing.T) {
	root := repositoryRoot(t)
	judged := 0
	err := filepath.WalkDir(filepath.Join(root, "examples"), func(file string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() || entry.Name() != "hil.conf" {
			return nil
		}
		relative, err := filepath.Rel(root, file)
		if err != nil {
			return err
		}
		spec, err := Load(root, relative)
		if err != nil {
			t.Errorf("%s: %v", relative, err)
			return nil
		}
		if err := checkExpectationIsNotAlsoForbidden(spec); err != nil {
			t.Errorf("%s: %v", relative, err)
		}
		judged++
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if judged < 200 {
		t.Fatalf("only %d HIL manifests judged", judged)
	}
}
