// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRulesCommentsWaiversAndMultilineCast(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "src", "boot.c")
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	source := "void SystemInit(void) {\n" +
		"  (void)\n    ra8_cgc_init();\n" +
		"  // (void)ra8_cgc_init();\n" +
		"  (void)ra8_tz_secure_boot_verify(); /* TZ-DISCARD-OK: reviewed fallback */\n" +
		"}\n"
	if err := os.WriteFile(path, []byte(source), 0o600); err != nil {
		t.Fatal(err)
	}
	findings := checkFile(filepath.Join("src", "boot.c"), root)
	if len(findings) != 1 || findings[0].rule != "B" || findings[0].line != 2 {
		t.Fatalf("checkFile findings = %+v", findings)
	}
}

func TestBuildOutputAndExemptionPolicy(t *testing.T) {
	for _, path := range []string{
		"examples/demo/build/main.c",
		"tools/tool/CMakeFiles/main.c",
		"libs/third_party/vendor.c",
		"libs/ra8_fonts/table.c",
	} {
		if !isBuildOutput(path) && !isExempt(path) {
			t.Errorf("path %q should be excluded", path)
		}
	}
	if hasAllowedExtension("libs/source.C") || hasAllowedExtension("libs/source.H") {
		t.Fatal("uppercase extensions must match the original case-sensitive scope")
	}
	for _, path := range []string{"scripts/build/helper.c", "libs/mybuild/source.c", "tools/builders/main.c"} {
		if isBuildOutput(path) {
			t.Errorf("path %q should stay in scope", path)
		}
	}
}

func TestRunSelfTestAndBadArguments(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if got := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr); got != 0 {
		t.Fatalf("self-test exit=%d stdout=%s stderr=%s", got, stdout.String(), stderr.String())
	}
	stdout.Reset()
	stderr.Reset()
	if got := Run(context.Background(), t.TempDir(), []string{"--bad"}, &stdout, &stderr); got != 2 {
		t.Fatalf("bad argument exit=%d", got)
	}
	if !strings.Contains(stderr.String(), "usage:") {
		t.Fatalf("bad argument diagnostic = %q", stderr.String())
	}
}

func TestDiscoverHonorsRootsAndExemptions(t *testing.T) {
	root := t.TempDir()
	for _, rel := range []string{
		"libs/core/a.c", "apps/product/main.cpp", "examples/demo/build/out.c",
		"libs/third_party/vendor.c", "libs/ra8_fonts/font.c", "docs/source.c",
	} {
		path := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(""), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	files, err := discover(context.Background(), root)
	if err != nil {
		t.Fatal(err)
	}
	got := strings.Join(files, ",")
	if got != "apps/product/main.cpp,libs/core/a.c" {
		t.Fatalf("discover() = %q", got)
	}
}

func TestAbsoluteBuildOutputIsFiltered(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "examples", "app", "build", "bad.c")
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("void f(void) { (void)ra8_tz_secure_boot_verify(); }\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	if got := Run(context.Background(), root, []string{path}, &stdout, &stderr); got != 0 {
		t.Fatalf("build output scan exit=%d stdout=%s stderr=%s", got, stdout.String(), stderr.String())
	}
}
