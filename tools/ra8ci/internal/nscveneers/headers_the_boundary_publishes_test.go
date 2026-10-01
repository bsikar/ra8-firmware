// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// tree writes a checkout with the given public headers and NSC sources and
// returns its root.
func tree(t *testing.T, headers, sources map[string]string) string {
	t.Helper()
	root := t.TempDir()
	incDir := filepath.Join(root, "libs", "ra8_nsc", "inc")
	srcDir := filepath.Join(root, "libs", "ra8_nsc", "src")
	if err := os.MkdirAll(incDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(srcDir, 0o755); err != nil {
		t.Fatal(err)
	}
	for name, body := range headers {
		if err := os.WriteFile(filepath.Join(incDir, name), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	for name, body := range sources {
		if err := os.WriteFile(filepath.Join(srcDir, name), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

// scan runs the gate over root and returns its exit code and combined output.
func scan(t *testing.T, root string) (int, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, nil, &stdout, &stderr)
	return code, stdout.String() + stderr.String()
}

func TestPublicHeadersListsEveryHeaderInTheIncludeDirectory(t *testing.T) {
	root := tree(t, map[string]string{
		"ra8_nsc.h":       "",
		"ra8_nsc_io.h":    "",
		"ra8_nsc_comms.h": "",
	}, nil)
	got, err := publicHeaders(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 3 {
		t.Fatalf("publicHeaders() = %v, want three headers", got)
	}
}

func TestPublicHeadersReturnsRepositoryRelativeSlashPaths(t *testing.T) {
	root := tree(t, map[string]string{"ra8_nsc.h": ""}, nil)
	got, err := publicHeaders(root)
	if err != nil {
		t.Fatal(err)
	}
	if want := headerDir + "/ra8_nsc.h"; got[0] != want {
		t.Fatalf("publicHeaders() = %q, want %q", got[0], want)
	}
}

func TestPublicHeadersIgnoresNonHeaderFiles(t *testing.T) {
	root := tree(t, map[string]string{"ra8_nsc.h": "", "README.md": "", "ra8_nsc.c": ""}, nil)
	got, err := publicHeaders(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 {
		t.Fatalf("publicHeaders() = %v, want only the header", got)
	}
}

func TestPublicHeadersFailsWhenTheIncludeDirectoryIsAbsent(t *testing.T) {
	if _, err := publicHeaders(t.TempDir()); err == nil {
		t.Fatal("publicHeaders() accepted a checkout with no NSC include directory")
	}
}

func TestAVeneerDeclaredOutsideTheFirstHeaderIsChecked(t *testing.T) {
	root := tree(t, map[string]string{
		"ra8_nsc.h":    "RA8_NSC_VENEER void ra8_nsc_defined(void);\n",
		"ra8_nsc_io.h": "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n",
	}, map[string]string{
		"nsc.c": "RA8_NSC_VENEER void ra8_nsc_defined(void) {}\nvoid caller(void) { ra8_nsc_phantom(); }\n",
	})
	code, out := scan(t, root)
	if code != 1 {
		t.Fatalf("exit=%d, want 1; output=%s", code, out)
	}
	if !strings.Contains(out, "ra8_nsc_phantom") {
		t.Fatalf("output did not name the phantom veneer: %s", out)
	}
}

func TestAMissingDefinitionNamesTheHeaderThatDeclaredIt(t *testing.T) {
	root := tree(t, map[string]string{
		"ra8_nsc.h":       "RA8_NSC_VENEER void ra8_nsc_defined(void);\n",
		"ra8_nsc_comms.h": "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n",
	}, map[string]string{
		"nsc.c": "RA8_NSC_VENEER void ra8_nsc_defined(void) {}\n",
	})
	_, out := scan(t, root)
	if !strings.Contains(out, "declared in "+headerDir+"/ra8_nsc_comms.h") {
		t.Fatalf("finding did not name the declaring header: %s", out)
	}
}

func TestEveryHeaderDefinedStaysQuiet(t *testing.T) {
	root := tree(t, map[string]string{
		"ra8_nsc.h":    "RA8_NSC_VENEER void ra8_nsc_one(void);\n",
		"ra8_nsc_io.h": "RA8_NSC_VENEER void ra8_nsc_two(void);\n",
	}, map[string]string{
		"nsc.c": "RA8_NSC_VENEER void ra8_nsc_one(void) {}\nRA8_NSC_VENEER void ra8_nsc_two(void) {}\n",
	})
	code, out := scan(t, root)
	if code != 0 {
		t.Fatalf("exit=%d, want 0; output=%s", code, out)
	}
}

func TestThePassLineCountsTheHeadersItRead(t *testing.T) {
	root := tree(t, map[string]string{
		"ra8_nsc.h":    "RA8_NSC_VENEER void ra8_nsc_one(void);\n",
		"ra8_nsc_io.h": "RA8_NSC_VENEER void ra8_nsc_two(void);\n",
		"ra8_nsc_x.h":  "/* no veneers here */\n",
	}, map[string]string{
		"nsc.c": "RA8_NSC_VENEER void ra8_nsc_one(void) {}\nRA8_NSC_VENEER void ra8_nsc_two(void) {}\n",
	})
	_, out := scan(t, root)
	if !strings.Contains(out, "all 2 RA8_NSC_VENEER declaration(s) across 3 header(s)") {
		t.Fatalf("pass line did not report the scope it read: %s", out)
	}
}

func TestOneNameDeclaredInTwoHeadersIsReportedOnce(t *testing.T) {
	root := tree(t, map[string]string{
		"ra8_nsc.h":    "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n",
		"ra8_nsc_io.h": "RA8_NSC_VENEER void ra8_nsc_phantom(void);\n",
	}, map[string]string{"nsc.c": "void caller(void) { ra8_nsc_phantom(); }\n"})
	_, out := scan(t, root)
	if got := strings.Count(out, "ra8_nsc_phantom:"); got != 1 {
		t.Fatalf("phantom reported %d times, want once: %s", got, out)
	}
	if !strings.Contains(out, "declared in "+headerDir+"/ra8_nsc.h") {
		t.Fatalf("duplicate name did not keep the first header it was seen in: %s", out)
	}
}

func TestACheckoutWithNoPublicHeaderIsRefused(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "libs", "ra8_nsc", "inc"), 0o755); err != nil {
		t.Fatal(err)
	}
	code, out := scan(t, root)
	if code != 1 {
		t.Fatalf("exit=%d, want 1; output=%s", code, out)
	}
	if !strings.Contains(out, "no public header found") {
		t.Fatalf("refusal did not say why: %s", out)
	}
}
