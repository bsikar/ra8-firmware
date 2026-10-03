// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
	"golang.org/x/sys/windows"
)

func TestCheckAcceptsOwnerOnlyDACL(t *testing.T) {
	path := filepath.Join(t.TempDir(), "secret")
	if err := os.WriteFile(path, []byte("secret"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := testprivatefile.OwnerOnly(path); err != nil {
		t.Fatal(err)
	}
	if err := Check(path); err != nil {
		t.Fatalf("owner-only DACL refused: %v", err)
	}
}

func TestCheckRefusesEveryoneDACL(t *testing.T) {
	path := filepath.Join(t.TempDir(), "secret")
	if err := os.WriteFile(path, []byte("secret"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := testprivatefile.OwnerOnly(path); err != nil {
		t.Fatal(err)
	}
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		t.Fatal(err)
	}
	owner := user.User.Sid.String()
	sd, err := windows.SecurityDescriptorFromString("D:P(A;;FA;;;" + owner + ")(A;;FA;;;WD)")
	if err != nil {
		t.Fatal(err)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		t.Fatal(err)
	}
	if err := windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION,
		nil, nil, dacl, nil); err != nil {
		t.Fatal(err)
	}
	if err := Check(path); err == nil {
		t.Fatal("Everyone ACE was accepted")
	}
}
