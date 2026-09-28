// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package stubcryptoguard

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// What checkFile decides is held elsewhere. What is pinned here is the run
// around it: whether a whole tree of reviewed translation units is answered
// with the right exit status, and whether the report a failing run prints is
// one an engineer can act on without reading this gate's source.

func plantStubTree(t *testing.T, contents func(item stub) string) string {
	t.Helper()
	root := t.TempDir()
	for _, item := range stubTranslationUnits {
		body := contents(item)
		if body == "" {
			continue
		}
		full := filepath.Join(root, filepath.FromSlash(item.path))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatalf("plant %s: %v", item.path, err)
		}
		if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
			t.Fatalf("plant %s: %v", item.path, err)
		}
	}
	return root
}

func guardedStub(token string) string {
	return "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n" +
		"static int " + token + "(void) { return 0; }\n" +
		"#else\n" +
		"return k_ra8_err_unsupported;\n" +
		"#endif\n"
}

func ran(t *testing.T, root string, args ...string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestATreeOfGuardedStubsPassesAndSaysHowManyItRead(t *testing.T) {
	root := plantStubTree(t, func(item stub) string { return guardedStub(item.token) })
	code, stdout, stderr := ran(t, root)
	if code != 0 {
		t.Fatalf("exit %d, want 0: %s%s", code, stdout, stderr)
	}
	if !strings.Contains(stdout, "PASS") {
		t.Errorf("stdout %q does not report the pass", stdout)
	}
	if !strings.Contains(stdout, "8 stub crypto TU(s)") {
		t.Errorf("stdout %q does not name how many units were actually read; a pass over nothing reads the same as a pass over all of them", stdout)
	}
	if stderr != "" {
		t.Errorf("a clean run wrote %q to stderr", stderr)
	}
}

func TestAMissingTranslationUnitIsAFindingNotASkip(t *testing.T) {
	root := plantStubTree(t, func(item stub) string { return "" })
	code, stdout, _ := ran(t, root)
	if code != 1 {
		t.Fatalf("exit %d, want 1: a tree with none of the reviewed units cannot pass", code)
	}
	for _, item := range stubTranslationUnits {
		if !strings.Contains(stdout, item.path) {
			t.Errorf("the report does not name the absent %s", item.path)
		}
	}
	if !strings.Contains(stdout, "file not found") {
		t.Errorf("stdout %q does not say the units are missing", stdout)
	}
	if strings.Contains(stdout, "PASS") {
		t.Errorf("stdout %q reports a pass alongside its findings", stdout)
	}
}

func TestOneUnguardedUnitFailsTheWholeRunAndSaysHowToFixIt(t *testing.T) {
	broken := stubTranslationUnits[3]
	root := plantStubTree(t, func(item stub) string {
		if item.path == broken.path {
			return "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n" +
				"static int " + item.token + "(void) { return 0; }\n" +
				"#else\n" +
				"return k_ra8_ok;\n" +
				"#endif\n"
		}
		return guardedStub(item.token)
	})
	code, stdout, _ := ran(t, root)
	if code != 1 {
		t.Fatalf("exit %d, want 1: one production arm answering k_ra8_ok is the whole point of this gate", code)
	}
	if !strings.Contains(stdout, broken.path) || !strings.Contains(stdout, "not fail-closed") {
		t.Errorf("the report does not name the unit at fault or why: %q", stdout)
	}
	for _, item := range stubTranslationUnits {
		if item.path != broken.path && strings.Contains(stdout, item.path) {
			t.Errorf("a guarded unit %s was reported alongside the broken one", item.path)
		}
	}
	for _, instruction := range []string{
		"RA8_INSECURE_STUB_CRYPTO",
		"k_ra8_err_",
		"#error",
	} {
		if !strings.Contains(stdout, instruction) {
			t.Errorf("the remediation does not mention %s: %q", instruction, stdout)
		}
	}
}

func TestAUnitThatCannotBeDecodedIsRefusedRatherThanScanned(t *testing.T) {
	first := stubTranslationUnits[0]
	root := plantStubTree(t, func(item stub) string {
		if item.path == first.path {
			return ""
		}
		return guardedStub(item.token)
	})
	full := filepath.Join(root, filepath.FromSlash(first.path))
	if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
		t.Fatalf("plant: %v", err)
	}
	if err := os.WriteFile(full, []byte{0xff, 0xfe, 0xff}, 0o644); err != nil {
		t.Fatalf("plant: %v", err)
	}
	code, stdout, _ := ran(t, root)
	if code != 1 {
		t.Fatalf("exit %d, want 1", code)
	}
	if !strings.Contains(stdout, "cannot decode UTF-8") {
		t.Errorf("stdout %q does not say the unit is undecodable; a byte soup that happens to hold no token would otherwise read as guarded", stdout)
	}
}

func TestAnInsecureTokenOutsideItsGuardIsNamedByLine(t *testing.T) {
	leaking := stubTranslationUnits[1]
	root := plantStubTree(t, func(item stub) string {
		if item.path == leaking.path {
			return guardedStub(item.token) + "static int " + item.token + "_copy;\n"
		}
		return guardedStub(item.token)
	})
	code, stdout, _ := ran(t, root)
	if code != 1 {
		t.Fatalf("exit %d, want 1", code)
	}
	if !strings.Contains(stdout, "OUTSIDE") || !strings.Contains(stdout, "line 6") {
		t.Errorf("the report does not place the escaped token: %q", stdout)
	}
}

func TestACancelledRunIsRefusedRatherThanReportedClean(t *testing.T) {
	root := plantStubTree(t, func(item stub) string { return guardedStub(item.token) })
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var stdout, stderr bytes.Buffer
	if code := Run(ctx, root, nil, &stdout, &stderr); code != 2 {
		t.Fatalf("exit %d, want 2: a cancelled scan read nothing and must not answer 0", code)
	}
	if !strings.Contains(stderr.String(), "cancelled") {
		t.Errorf("stderr %q does not report the cancellation", stderr.String())
	}
	if strings.Contains(stdout.String(), "PASS") {
		t.Errorf("stdout %q reports a pass over a cancelled scan", stdout.String())
	}
}

func TestAnArgumentThisGateDoesNotTakeIsRefusedBeforeAnyScan(t *testing.T) {
	root := plantStubTree(t, func(item stub) string { return guardedStub(item.token) })
	for _, args := range [][]string{
		{"--all"},
		{"--selftest", "--selftest"},
		{"libs/ra8_hal/src/ra8_rsip_ecc.c"},
		{"--selftest", "extra"},
		{"-x"},
	} {
		code, stdout, stderr := ran(t, root, args...)
		if code != 2 {
			t.Errorf("%v: exit %d, want 2", args, code)
		}
		if !strings.Contains(stderr, "usage:") {
			t.Errorf("%v: stderr %q does not show the usage", args, stderr)
		}
		if stdout != "" {
			t.Errorf("%v: wrote %q to stdout for an invocation it refused", args, stdout)
		}
	}
}

func TestTheSelfTestProvesBothDirections(t *testing.T) {
	code, stdout, stderr := ran(t, t.TempDir(), "--selftest")
	if code != 0 {
		t.Fatalf("exit %d, want 0: %s%s", code, stdout, stderr)
	}
	if !strings.Contains(stdout, "both directions") {
		t.Errorf("stdout %q does not report the self-test passing", stdout)
	}
	for _, direction := range []string{
		"[ok] guarded token plus hard-error branch stays quiet",
		"[ok] non-failing else and escaped insecure token both fire",
	} {
		if !strings.Contains(stdout, direction) {
			t.Errorf("the self-test did not report %q: %q", direction, stdout)
		}
	}
	if stderr != "" {
		t.Errorf("a passing self-test wrote %q to stderr", stderr)
	}
}

func TestTheSelfTestDoesNotDependOnTheRootItIsGiven(t *testing.T) {
	// The self-test builds its own fixture, so it must answer the same over a
	// root with no source tree at all as it does over the repository.
	first, _, _ := ran(t, t.TempDir(), "--selftest")
	second, _, _ := ran(t, filepath.Join(t.TempDir(), "absent"), "--selftest")
	if first != 0 || second != 0 {
		t.Fatalf("self-test answered %d and %d, want 0 and 0", first, second)
	}
}
