// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestTerraformWorkspacePathAndPrivateFileFence(t *testing.T) {
	root := t.TempDir()
	workspace := filepath.Join(root, "reservation")
	if err := os.Mkdir(workspace, 0o700); err != nil {
		t.Fatal(err)
	}
	privateFile := filepath.Join(workspace, "variables.tfvars.json")
	if err := os.WriteFile(privateFile, []byte("{\"runner\":{}}"), 0o600); err != nil {
		t.Fatal(err)
	}
	if !pathInside(workspace, privateFile) || pathInside(workspace, root) ||
		pathInside(workspace, workspace) {
		t.Fatal("workspace path containment returned an unexpected result")
	}
	if err := requirePrivateFileInside(workspace, privateFile, 1024); err != nil {
		t.Fatalf("accepted private workspace file: %v", err)
	}
	if err := requirePrivateFileInside(workspace, filepath.Join(root, "outside"), 1024); err == nil {
		t.Fatal("accepted a variable file outside the reservation workspace")
	}
	if err := os.Chmod(privateFile, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := requirePrivateFileInside(workspace, privateFile, 1024); err == nil {
		t.Fatal("accepted a variable file readable by other users")
	}
}

func TestTerraformWorkspaceRejectsSymlinksAndBoundsOutput(t *testing.T) {
	root := t.TempDir()
	workspace := filepath.Join(root, "reservation")
	if err := os.Mkdir(workspace, 0o700); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(root, "target")
	if err := os.WriteFile(target, []byte("data"), 0o600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(workspace, "linked")
	if err := os.Symlink(target, link); err != nil {
		t.Fatal(err)
	}
	if err := requirePrivateFileInside(workspace, link, 1024); err == nil {
		t.Fatal("accepted a symlinked Terraform input")
	}
	buffer := &boundedTerraformBuffer{limit: 4}
	if n, err := buffer.Write([]byte("four")); err != nil || n != 4 {
		t.Fatalf("bounded write = %d, %v", n, err)
	}
	if n, err := buffer.Write([]byte("x")); err == nil || n != 0 {
		t.Fatalf("output limit did not fail: n=%d err=%v", n, err)
	}
	if strings.Contains(string(buffer.data), "secret") {
		t.Fatal("unexpected secret in bounded output")
	}
}
