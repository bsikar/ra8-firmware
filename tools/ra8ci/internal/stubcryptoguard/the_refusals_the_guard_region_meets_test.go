// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package stubcryptoguard

import (
	"bytes"
	"strings"
	"testing"
)

// theGuard is the opening line this gate looks for, spelled the one way it
// accepts: both names present on a single #if.
const theGuard = "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)"

// A source with no guard at all is the ordinary case for most of the tree, and
// it must answer "no guard here" rather than point at line zero as though a
// region had been found.
func TestNoGuardRegionIsFoundWhereNoGuardOpens(t *testing.T) {
	for _, source := range []string{
		"static int plain;\n",
		"#if defined(RA8_OFF_TARGET)\nstatic int half;\n#endif\n",
		"#if defined(RA8_INSECURE_STUB_CRYPTO)\nstatic int other;\n#endif\n",
		"// RA8_INSECURE_STUB_CRYPTO and RA8_OFF_TARGET in prose, not a directive\n",
	} {
		start, insecureEnd, end, ok := guardRegion(strings.Split(source, "\n"))
		if ok {
			t.Errorf("guardRegion found a region in %q", source)
		}
		if start != 0 || insecureEnd != 0 || end != 0 {
			t.Errorf("a refused region still answered %d/%d/%d for %q", start, insecureEnd, end, source)
		}
	}
}

// An #if that never closes is the shape a bad edit leaves behind. Reading it as
// a guard would hand the rest of the file to the insecure arm, so the region is
// refused rather than run to the end of the file.
func TestAnUnterminatedGuardIsNotAGuard(t *testing.T) {
	for name, source := range map[string]string{
		"no endif at all":        theGuard + "\nstatic int body;\n",
		"an arm but no endif":    theGuard + "\nstatic int body;\n#else\nreturn k_ra8_err_unsupported;\n",
		"inner endif only":       theGuard + "\n#if defined(NESTED)\nstatic int body;\n#endif\n#else\nreturn k_ra8_err_unsupported;\n",
		"nested if never closes": theGuard + "\n#else\nreturn k_ra8_err_unsupported;\n#if defined(NESTED)\n#endif\n",
	} {
		start, insecureEnd, end, ok := guardRegion(strings.Split(source, "\n"))
		if ok {
			t.Errorf("%s: guardRegion accepted an unterminated guard", name)
		}
		if start != 0 || insecureEnd != 0 || end != 0 {
			t.Errorf("%s: a refused region still answered %d/%d/%d", name, start, insecureEnd, end)
		}
	}
}

// The self-test builds its own fixture in a temporary directory. When that
// directory cannot be made it must say so and fail, rather than report on a
// fixture it never wrote.
func TestTheSelfTestRefusesWhenItCannotBuildItsFixture(t *testing.T) {
	setMissingTempDirectory(t)

	var stdout, stderr bytes.Buffer
	if selfTest(&stdout, &stderr) {
		t.Fatal("the self-test passed without a fixture to test against")
	}
	if !strings.Contains(stderr.String(), "create self-test fixture") {
		t.Fatalf("the refusal does not name what it could not build: %q", stderr.String())
	}
	if strings.Contains(stdout.String(), "[ok]") {
		t.Fatalf("a self-test that built nothing still reported a passing case: %q", stdout.String())
	}
}

// A self-test that cannot run is a failure, not a pass and not an argument
// error: the gate answers 1, the code that says its own checks did not hold.
func TestRunAnswersOneWhenTheSelfTestCannotRun(t *testing.T) {
	root := t.TempDir()
	setMissingTempDirectory(t)

	code, stdout, stderr := ran(t, root, "--selftest")
	if code != 1 {
		t.Fatalf("exit %d, want 1: %s%s", code, stdout, stderr)
	}
	if strings.Contains(stdout, "both directions") {
		t.Fatalf("a self-test that never ran claimed to pass: %q", stdout)
	}
}
