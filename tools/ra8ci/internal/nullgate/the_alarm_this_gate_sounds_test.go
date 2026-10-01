// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nullgate

import (
	"context"
	"regexp"
	"strings"
	"testing"
)

// ranSelfTest answers the gate's --selftest over a root it never reads: the
// self-test builds its own fixture in a temporary directory, so the root is
// only there to satisfy the invocation.
func ranSelfTest(t *testing.T) (int, string, string) {
	t.Helper()
	var out, errs strings.Builder
	code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &out, &errs)
	return code, out.String(), errs.String()
}

// withoutTheRegisteredGeneratedSource drops the one entry the self-test relies
// on to prove a registered generated file is exempt, and puts it back after.
func withoutTheRegisteredGeneratedSource(t *testing.T) {
	t.Helper()
	original := generatedPaths
	t.Cleanup(func() { generatedPaths = original })
	trimmed := make(map[string]bool, len(original))
	for path, exempt := range original {
		if path == "libs/ra8_c6link/src/ra8_media_download.pb-c.c" {
			continue
		}
		trimmed[path] = exempt
	}
	generatedPaths = trimmed
}

// The self-test is what stands between a scope rule that quietly stopped
// applying and a gate that goes on reporting a clean tree. With the generated
// source de-registered, the assertion that covers it has to fail by name, the
// run has to answer 1, and the summary has to say how many assertions broke.
func TestASelfTestWhoseScopeRuleWentMissingFails(t *testing.T) {
	withoutTheRegisteredGeneratedSource(t)

	code, out, errs := ranSelfTest(t)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(out, "[FAIL] registered generated source is exempt") {
		t.Errorf("stdout = %q, want the broken assertion marked FAIL by name", out)
	}
	if !strings.Contains(errs, "SELFTEST FAILED: 1 assertion(s)") {
		t.Errorf("stderr = %q, want the failure summary with its count", errs)
	}
	if strings.Contains(out, "all assertions held") {
		t.Errorf("stdout = %q, a failed self-test may not claim every assertion held", out)
	}
}

// The self-test judges every assertion rather than stopping at the first
// break, so two broken pieces are reported as two. A run that stopped early
// would send someone back for a second round after each fix.
func TestASelfTestReportsEveryBrokenAssertionNotJustTheFirst(t *testing.T) {
	originalToken := nullToken
	t.Cleanup(func() { nullToken = originalToken })
	nullToken = regexp.MustCompile(`\bZZZZ_NEVER_MATCHES\b`)
	withoutTheRegisteredGeneratedSource(t)

	code, out, errs := ranSelfTest(t)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(out, "[FAIL] bare NULL in code fires") {
		t.Errorf("stdout = %q, want the blinded detector marked FAIL", out)
	}
	if !strings.Contains(out, "[FAIL] registered generated source is exempt") {
		t.Errorf("stdout = %q, want the later assertion judged too", out)
	}
	if !strings.Contains(errs, "SELFTEST FAILED: 2 assertion(s)") {
		t.Errorf("stderr = %q, want both failures counted", errs)
	}
	if !strings.Contains(out, "[ok] nullptr / vendor macro / comment / string literal stays quiet") &&
		!strings.Contains(out, "[ok] nullptr") {
		t.Errorf("stdout = %q, want the assertions that still hold marked ok", out)
	}
}
