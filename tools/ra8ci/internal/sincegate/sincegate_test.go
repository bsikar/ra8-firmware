// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestValueAndPresenceChecks(t *testing.T) {
	version := "1.2.3"
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.c")
	if err := os.WriteFile(bad, []byte("/** @since Version 9.8.7 */\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if got := checkValues(bad, version); len(got) != 1 || !strings.Contains(got[0], "@since 9.8.7") {
		t.Fatalf("wrong version findings = %v", got)
	}
	good := filepath.Join(dir, "good.c")
	if err := os.WriteFile(good, []byte("/** @since "+version+" */\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if got := checkValues(good, version); len(got) != 0 {
		t.Fatalf("matching version findings = %v", got)
	}
	missing := filepath.Join(dir, "public.h")
	if err := os.WriteFile(missing, []byte("ra8_err_t ra8_public(void);\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if got := checkPresence(missing); len(got) != 1 {
		t.Fatalf("missing public API tag findings = %v", got)
	}
}

func TestPresenceLookbackLimit(t *testing.T) {
	dir := t.TempDir()
	header := filepath.Join(dir, "lookback.h")
	var lines []string
	lines = append(lines, "/** @since 1.0.0 */")
	for len(lines) < 30 {
		lines = append(lines, "/* comment */")
	}
	lines = append(lines, "ra8_err_t ra8_in_range(void);")
	if err := os.WriteFile(header, []byte(strings.Join(lines, "\n")), 0600); err != nil {
		t.Fatal(err)
	}
	if got := checkPresence(header); len(got) != 0 {
		t.Fatalf("tag at 30-line boundary rejected: %v", got)
	}
	lines = append(lines[:len(lines)-1], append([]string{"/* comment */"}, lines[len(lines)-1:]...)...)
	if err := os.WriteFile(header, []byte(strings.Join(lines, "\n")), 0600); err != nil {
		t.Fatal(err)
	}
	if got := checkPresence(header); len(got) != 1 {
		t.Fatalf("tag outside 30-line lookback accepted: %v", got)
	}
}

func TestRunRejectsBadArgumentsAndSelfTestsLiveScope(t *testing.T) {
	root := filepath.Clean(filepath.Join("..", "..", "..", ".."))
	var out, stderr bytes.Buffer
	if code := Run(nil, root, []string{"--all"}, &out, &stderr); code != 2 {
		t.Fatalf("nil context code = %d", code)
	}
	if code := Run(context.Background(), root, nil, &out, &stderr); code != 2 {
		t.Fatalf("empty args code = %d", code)
	}
	if code := Run(context.Background(), root, []string{"--selftest"}, &out, &stderr); code != 0 {
		t.Fatalf("selftest code = %d: %s", code, stderr.String())
	}
}
