// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package assertcasts

import (
	"bytes"
	"context"
	"regexp"
	"strings"
	"testing"
)

// withCastPattern swaps the one expression this gate reads its findings from,
// so the self-test is asked whether it notices its own detector going wrong.
func withCastPattern(t *testing.T, pattern string) {
	t.Helper()
	shipped := cast
	cast = regexp.MustCompile(pattern)
	t.Cleanup(func() { cast = shipped })
}

func selfTested(t *testing.T) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestTheSelfTestPassesOnThePatternThatShipped(t *testing.T) {
	code, stdout, stderr := selfTested(t)
	if code != 0 {
		t.Fatalf("shipped pattern: exit %d, stderr %q", code, stderr)
	}
	if !strings.Contains(stdout, "all cases pass (both directions).") {
		t.Fatalf("shipped pattern: stdout %q", stdout)
	}
	if strings.Contains(stdout, "[FAIL]") {
		t.Fatalf("shipped pattern reported a failing case: %q", stdout)
	}
}

func TestASelfTestWithABlindPatternCountsTheCastsItStoppedSeeing(t *testing.T) {
	withCastPattern(t, `^\s*\(__never_written_as_a_cast__\)`)
	code, stdout, stderr := selfTested(t)
	if code != 1 {
		t.Fatalf("blind pattern: exit %d, stderr %q", code, stderr)
	}
	// Every case that expects a finding loses it; the quiet case stays quiet,
	// which is exactly why a one-directional self-test would have passed.
	for _, want := range []string{
		"[FAIL] leading casts on both arguments fire",
		"[FAIL] raw-text matching includes comments and strings",
		"[FAIL] top-level comma splitting and malformed calls",
		"[ok] clean and nested casts stay quiet",
	} {
		if !strings.Contains(stdout, want) {
			t.Fatalf("blind pattern: stdout %q lacks %q", stdout, want)
		}
	}
	if !strings.Contains(stderr, "--selftest: 3 failure(s)") {
		t.Fatalf("blind pattern: stderr %q", stderr)
	}
	if strings.Contains(stdout, "all cases pass") {
		t.Fatalf("blind pattern still announced a pass: %q", stdout)
	}
}

func TestASelfTestWithAnIndiscriminatePatternCountsTheArgumentsItAccused(t *testing.T) {
	withCastPattern(t, `^`)
	code, stdout, stderr := selfTested(t)
	if code != 1 {
		t.Fatalf("indiscriminate pattern: exit %d, stderr %q", code, stderr)
	}
	// The other direction: a pattern that matches every argument keeps the
	// case that wanted findings and loses the three that bound them.
	for _, want := range []string{
		"[ok] leading casts on both arguments fire",
		"[FAIL] clean and nested casts stay quiet",
		"[FAIL] raw-text matching includes comments and strings",
		"[FAIL] top-level comma splitting and malformed calls",
	} {
		if !strings.Contains(stdout, want) {
			t.Fatalf("indiscriminate pattern: stdout %q lacks %q", stdout, want)
		}
	}
	if !strings.Contains(stderr, "--selftest: 3 failure(s)") {
		t.Fatalf("indiscriminate pattern: stderr %q", stderr)
	}
}

func TestEverySpelledWidthIsACastAndItsNeighboursAreNot(t *testing.T) {
	fires := []string{
		"(int)", "(size_t)", "(ssize_t)",
		"(int8_t)", "(int16_t)", "(int32_t)", "(int64_t)", "(int_t)",
		"(uint8_t)", "(uint16_t)", "(uint32_t)", "(uint64_t)", "(uint_t)",
	}
	for _, spelling := range fires {
		findings := scan("TEST_ASSERT_EQ("+spelling+"value, expected);\n", "t.c")
		if len(findings) != 1 || !strings.Contains(findings[0], "first arg") {
			t.Fatalf("%s: findings %v", spelling, findings)
		}
	}
	quiet := []string{
		"(float)", "(char)", "(uint128_t)", "(long)", "(void *)", "(myint)",
	}
	for _, spelling := range quiet {
		if findings := scan("TEST_ASSERT_EQ("+spelling+"value, expected);\n", "t.c"); len(findings) != 0 {
			t.Fatalf("%s should not be read as a width cast: %v", spelling, findings)
		}
	}
	// The cast has to lead the argument: the same text further in is the
	// nested conversion the gate deliberately allows.
	if findings := scan("TEST_ASSERT_EQ(load((int)value), expected);\n", "t.c"); len(findings) != 0 {
		t.Fatalf("a nested conversion is not a leading cast: %v", findings)
	}
}
