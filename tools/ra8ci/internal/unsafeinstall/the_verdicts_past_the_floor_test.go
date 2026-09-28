// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package unsafeinstall

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Everything past the scope floor. The floor is 4000 files, which looked
// too expensive to fixture and is not: empty files cost about a second,
// and the verdicts this gate exists to give are only reachable through it.

func plantFullScope(t *testing.T, extra map[string]string) (string, int) {
	t.Helper()
	files := make(map[string]string, minimumScopedFiles+len(extra)+1)
	files[gateSource] = "package unsafeinstall\n"
	for index := 0; index < minimumScopedFiles; index++ {
		files["apps/unit"+decimal(index)+".py"] = ""
	}
	for rel, body := range extra {
		files[rel] = body
	}
	return plantRepo(t, files), len(files)
}

func decimal(value int) string {
	if value == 0 {
		return "0"
	}
	var digits []byte
	for value > 0 {
		digits = append([]byte{byte('0' + value%10)}, digits...)
		value /= 10
	}
	return string(digits)
}

func swept(t *testing.T, root string) (int, string, string) {
	t.Helper()
	var out, errs bytes.Buffer
	code := Run(context.Background(), root, nil, &out, &errs)
	return code, out.String(), errs.String()
}

// A real scope with no override is the only case that may report clean, and
// it says how many files it read: that count is what separates a real sweep
// from a collapsed one.
func TestAFullScopeWithNoOverrideReportsCleanAndItsCount(t *testing.T) {
	root, planted := plantFullScope(t, nil)
	code, out, errs := swept(t, root)
	if code != 0 {
		t.Fatalf("code = %d, want 0 (stderr %q)", code, errs)
	}
	if !strings.Contains(out, "clean ("+decimal(planted)+" first-party files)") {
		t.Fatalf("stdout = %q, want the clean verdict over %d files", out, planted)
	}
	if errs != "" {
		t.Fatalf("stderr = %q, want nothing", errs)
	}
}

// One override anywhere in that scope fails the gate, names the file and
// line, and carries the remedy. The verdict goes to stderr and stdout stays
// empty: a failing gate has no clean line to print.
func TestAnOverrideInAFullScopeFailsAndIsNamed(t *testing.T) {
	root, _ := plantFullScope(t, map[string]string{
		"infra/provision.sh": "#!/bin/sh\nset -eu\npython3 -m pip install " + forbidden + " libclang\n",
	})
	code, out, errs := swept(t, root)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(errs, "unsafe system-Python package override found:") {
		t.Fatalf("stderr = %q, want the failure header", errs)
	}
	if !strings.Contains(errs, "infra/provision.sh:3") {
		t.Fatalf("stderr = %q, want the path and line", errs)
	}
	if !strings.Contains(errs, "Create a venv and wire its interpreter/PATH explicitly.") {
		t.Fatalf("stderr = %q, want the remedy", errs)
	}
	if out != "" {
		t.Fatalf("stdout = %q, want no clean verdict", out)
	}
}

// Every offending line is reported, in scope order, across files as well as
// within one: a gate that named only the first would send an author round
// the loop once per line.
func TestEveryOffendingLineInEveryFileIsReported(t *testing.T) {
	root, _ := plantFullScope(t, map[string]string{
		"infra/one.sh": "python3 -m pip install " + forbidden + " a\nsafe line\npython3 -m pip install " + forbidden + " b\n",
		"infra/two.sh": "ENV PIP" + "_BREAK_SYSTEM_" + "PACKAGES=1\n",
	})
	code, _, errs := swept(t, root)
	if code != 1 {
		t.Fatalf("code = %d, want 1 (stderr %q)", code, errs)
	}
	for _, want := range []string{"infra/one.sh:1", "infra/one.sh:3", "infra/two.sh:1"} {
		if !strings.Contains(errs, want) {
			t.Fatalf("stderr = %q, missing %q", errs, want)
		}
	}
	if strings.Contains(errs, "infra/one.sh:2") {
		t.Fatalf("stderr = %q, a safe line is not a finding", errs)
	}
	if first, second := strings.Index(errs, "infra/one.sh:1"), strings.Index(errs, "infra/two.sh:1"); first > second {
		t.Fatalf("stderr = %q, want the findings in scope order", errs)
	}
}

// A file in scope that cannot be read is a hard failure naming the file,
// never a skip. Skipping it would let an unreadable script carry the
// override past a gate that reported success.
func TestAFileTheScanCannotReadIsRefusedNotSkipped(t *testing.T) {
	root, _ := plantFullScope(t, map[string]string{"infra/sealed.sh": "#!/bin/sh\n"})
	sealed := filepath.Join(root, "infra", "sealed.sh")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatalf("seal the file: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o644) })
	if handle, err := os.Open(sealed); err == nil {
		handle.Close()
		t.Skip("this box reads a 0o000 file, so the refusal cannot be reached here")
	}
	code, out, errs := swept(t, root)
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, "scan failed") || !strings.Contains(errs, "read infra/sealed.sh") {
		t.Fatalf("stderr = %q, want the scan and the file named", errs)
	}
	if strings.Contains(out, "clean") {
		t.Fatalf("stdout = %q, must never call an unread file clean", out)
	}
}

// The scan stops on a cancelled context and hands the cancellation back
// rather than returning the findings it happened to reach.
func TestTheScanStopsOnACancelledContext(t *testing.T) {
	root := plantTree(t, map[string]string{"infra/one.sh": "python3 -m pip install " + forbidden + " a\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	findings, err := scan(ctx, root, []string{"infra/one.sh"})
	if err == nil {
		t.Fatalf("scan returned %v, want the cancellation", findings)
	}
	if !strings.Contains(err.Error(), context.Canceled.Error()) {
		t.Fatalf("err = %v, want the cancellation named", err)
	}
	if findings != nil {
		t.Fatalf("findings = %v, want none", findings)
	}
}

// A file that is not valid UTF-8 is passed over rather than failing the
// scan: binary blobs live in first-party directories and are not text the
// override can hide in.
func TestTheScanPassesOverTextThatIsNotValidUTF8(t *testing.T) {
	root := plantTree(t, map[string]string{"infra/blob.bin": ""})
	body := append([]byte{0xff, 0xfe, 0xfd}, []byte("\npython3 -m pip install "+forbidden+" a\n")...)
	if err := os.WriteFile(filepath.Join(root, "infra", "blob.bin"), body, 0o644); err != nil {
		t.Fatalf("write the blob: %v", err)
	}
	findings, err := scan(context.Background(), root, []string{"infra/blob.bin"})
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	if len(findings) != 0 {
		t.Fatalf("findings = %v, want none from undecodable bytes", findings)
	}
}

// A scope that reaches the count but is missing the gate's own source is
// refused too. Both halves of that check matter: the gate scanning
// everything except itself is a scope that silently stopped working.
func TestAScopeWithoutTheGateSourceIsRefusedEvenAtFullCount(t *testing.T) {
	files := make(map[string]string, minimumScopedFiles)
	for index := 0; index < minimumScopedFiles; index++ {
		files["apps/unit"+decimal(index)+".py"] = ""
	}
	code, out, errs := swept(t, plantRepo(t, files))
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q)", code, out)
	}
	if !strings.Contains(errs, gateSource) {
		t.Fatalf("stderr = %q, want the missing gate source named", errs)
	}
	if strings.Contains(out, "clean") {
		t.Fatalf("stdout = %q, must not report clean", out)
	}
}
