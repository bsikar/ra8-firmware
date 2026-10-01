// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package stubcryptoguard

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func armsOf(t *testing.T, source string) ([]string, []int) {
	t.Helper()
	lines := strings.Split(source, "\n")
	ifIndex, insecureEnd, endIndex, ok := guardRegion(lines)
	if !ok {
		t.Fatalf("guardRegion did not find a guard in:\n%s", source)
	}
	if ifIndex != 0 {
		t.Fatalf("guard opened at %d, want 0", ifIndex)
	}
	return lines, armStarts(lines, insecureEnd, endIndex)
}

// checked writes source as the one stub translation unit under a temp root and
// returns what the gate says about it.
func checked(t *testing.T, source string) string {
	t.Helper()
	const rel = "libs/test/stub.c"
	root := t.TempDir()
	file := filepath.Join(root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(file), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(file, []byte(source), 0o600); err != nil {
		t.Fatal(err)
	}
	return strings.Join(checkFile(rel, "insecure_token", root), "\n")
}

func TestElifEndsTheInsecureArm(t *testing.T) {
	lines := strings.Split("#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#elif defined(RA8_SOMETHING_ELSE)\n"+
		"return k_ra8_ok;\n"+
		"#else\n"+
		"return k_ra8_err_unsupported;\n"+
		"#endif\n", "\n")
	ifIndex, insecureEnd, endIndex, ok := guardRegion(lines)
	if !ok || ifIndex != 0 || insecureEnd != 2 || endIndex != 6 {
		t.Fatalf("guardRegion = (%d,%d,%d,%v), want (0,2,6,true)", ifIndex, insecureEnd, endIndex, ok)
	}
}

func TestArmStartsListsEveryArm(t *testing.T) {
	_, starts := armsOf(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#elif defined(A)\n"+
		"#error no\n"+
		"#elif defined(B)\n"+
		"#error no\n"+
		"#else\n"+
		"#error no\n"+
		"#endif\n")
	want := []int{2, 4, 6}
	if len(starts) != len(want) {
		t.Fatalf("armStarts = %v, want %v", starts, want)
	}
	for i := range want {
		if starts[i] != want[i] {
			t.Fatalf("armStarts = %v, want %v", starts, want)
		}
	}
}

func TestArmStartsIgnoresNestedArms(t *testing.T) {
	_, starts := armsOf(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#else\n"+
		"#ifdef INNER\n"+
		"#elif defined(INNER_B)\n"+
		"#else\n"+
		"#endif\n"+
		"return k_ra8_err_unsupported;\n"+
		"#endif\n")
	if len(starts) != 1 || starts[0] != 2 {
		t.Fatalf("armStarts = %v, want [2]", starts)
	}
}

func TestArmStartsRefusesAnImpossibleRange(t *testing.T) {
	lines := []string{"#else", "#endif"}
	if got := armStarts(lines, -1, 1); got != nil {
		t.Fatalf("armStarts(-1) = %v, want nil", got)
	}
	if got := armStarts(lines, 1, 1); got != nil {
		t.Fatalf("armStarts(end==start) = %v, want nil", got)
	}
	if got := armStarts(lines, 0, 99); got != nil {
		t.Fatalf("armStarts(past end) = %v, want nil", got)
	}
}

func TestArmFailsClosedReadsBothRefusals(t *testing.T) {
	for _, body := range [][]string{
		{"#error placeholder crypto is not for production"},
		{"    return k_ra8_err_unsupported;"},
		{"", "\treturn k_ra8_err_not_implemented;"},
	} {
		if !armFailsClosed(body) {
			t.Fatalf("armFailsClosed(%q) = false, want true", body)
		}
	}
	for _, body := range [][]string{
		{"return k_ra8_ok;"},
		{},
		{"/* return k_ra8_ok and hope */"},
	} {
		if armFailsClosed(body) {
			t.Fatalf("armFailsClosed(%q) = true, want false", body)
		}
	}
}

func TestInsecureTokenInAnElifArmIsOutsideTheGuard(t *testing.T) {
	problems := checked(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#elif defined(RA8_ALLOW_WEAK)\n"+
		"static int insecure_token;\n"+
		"#else\n"+
		"return k_ra8_err_unsupported;\n"+
		"#endif\n")
	if !strings.Contains(problems, "OUTSIDE") || !strings.Contains(problems, "line 4") {
		t.Fatalf("an insecure token in an #elif arm was not reported: %q", problems)
	}
}

func TestEveryProductionArmMustFailClosed(t *testing.T) {
	problems := checked(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#elif defined(RA8_ALLOW_WEAK)\n"+
		"return k_ra8_ok;\n"+
		"#else\n"+
		"return k_ra8_err_unsupported;\n"+
		"#endif\n")
	if !strings.Contains(problems, "not fail-closed") || !strings.Contains(problems, "line 3") {
		t.Fatalf("a permissive #elif arm was not reported: %q", problems)
	}
}

func TestAFailClosedElifChainStaysQuiet(t *testing.T) {
	problems := checked(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#elif defined(RA8_ALLOW_WEAK)\n"+
		"#error placeholder crypto is not for production\n"+
		"#else\n"+
		"return k_ra8_err_unsupported;\n"+
		"#endif\n")
	if problems != "" {
		t.Fatalf("a fail-closed #elif chain fired: %q", problems)
	}
}

func TestAGuardWithOnlyElifArmsIsStillAGuard(t *testing.T) {
	problems := checked(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#elif defined(RA8_ALLOW_WEAK)\n"+
		"#error placeholder crypto is not for production\n"+
		"#endif\n")
	if problems != "" {
		t.Fatalf("a guard closed by #elif alone was rejected: %q", problems)
	}
}

func TestAGuardWithNoAlternateArmIsRejected(t *testing.T) {
	problems := checked(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#endif\n")
	if !strings.Contains(problems, "missing the stub-crypto guard") {
		t.Fatalf("a guard with no production arm was accepted: %q", problems)
	}
}

func TestTheOrdinaryElseOnlyGuardIsUnchanged(t *testing.T) {
	problems := checked(t, "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\n"+
		"static int insecure_token;\n"+
		"#else\n"+
		"return k_ra8_err_unsupported;\n"+
		"#endif\n")
	if problems != "" {
		t.Fatalf("the plain #if/#else guard fired: %q", problems)
	}
}
