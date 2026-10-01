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

func TestGuardRegionTracksNestedConditionals(t *testing.T) {
	lines := []string{
		"#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)",
		"#if INNER",
		"static int insecure;",
		"#else",
		"static int alternate;",
		"#endif",
		"#else",
		"return k_ra8_err_unsupported;",
		"#endif",
	}
	ifIndex, elseIndex, endIndex, ok := guardRegion(lines)
	if !ok || ifIndex != 0 || elseIndex != 6 || endIndex != 8 {
		t.Fatalf("guardRegion = (%d,%d,%d,%v)", ifIndex, elseIndex, endIndex, ok)
	}
}

func TestRunSelfTestAndUnknownArguments(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if got := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr); got != 0 {
		t.Fatalf("self-test exit=%d stdout=%s stderr=%s", got, stdout.String(), stderr.String())
	}
	if got := Run(context.Background(), t.TempDir(), []string{"unexpected"}, &stdout, &stderr); got != 2 {
		t.Fatalf("unknown arguments exit=%d", got)
	}
}

func TestRunReportsMissingAndEscapedStub(t *testing.T) {
	root := t.TempDir()
	item := stub{path: "libs/test/stub.c", token: "insecure_token"}
	file := filepath.Join(root, filepath.FromSlash(item.path))
	if err := os.MkdirAll(filepath.Dir(file), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(file, []byte("#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\nstatic int placeholder;\n#else\nreturn k_ra8_ok;\n#endif\nstatic int insecure_token;\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	problems := checkFile(item.path, item.token, root)
	joined := strings.Join(problems, "\n")
	if !strings.Contains(joined, "not fail-closed") || !strings.Contains(joined, "OUTSIDE") {
		t.Fatalf("checkFile problems = %v", problems)
	}
}
