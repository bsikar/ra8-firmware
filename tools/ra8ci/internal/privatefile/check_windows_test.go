// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
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

func TestRestrictFileRemovesTheLogonSessionACE(t *testing.T) {
	file, err := os.CreateTemp(t.TempDir(), "private-")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	if err := RestrictFile(file); err != nil {
		t.Fatalf("restrict newly created file: %v", err)
	}
	if err := CheckFile(file); err != nil {
		t.Fatalf("restricted file failed its check: %v", err)
	}
	sd, err := windows.GetNamedSecurityInfo(file.Name(), windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(sd.String(), "S-1-5-5-") {
		t.Fatalf("restricted DACL still contains a logon-session ACE: %s", sd.String())
	}
}

func TestCheckAcceptsOwnerSystemAndAdministrators(t *testing.T) {
	path := filepath.Join(t.TempDir(), "secret")
	if err := os.WriteFile(path, []byte("secret"), 0o600); err != nil {
		t.Fatal(err)
	}
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		t.Fatal(err)
	}
	system, err := windows.CreateWellKnownSid(windows.WinLocalSystemSid)
	if err != nil {
		t.Fatal(err)
	}
	admins, err := windows.CreateWellKnownSid(windows.WinBuiltinAdministratorsSid)
	if err != nil {
		t.Fatal(err)
	}
	sd, err := windows.SecurityDescriptorFromString("D:P(A;;FA;;;" + user.User.Sid.String() +
		")(A;;FA;;;" + system.String() + ")(A;;FA;;;" + admins.String() + ")")
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
	if err := Check(path); err != nil {
		t.Fatalf("owner, SYSTEM, Administrators DACL refused: %v", err)
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

func TestCheckRefusesOtherBroadPrincipals(t *testing.T) {
	for _, sidType := range []windows.WELL_KNOWN_SID_TYPE{
		windows.WinBuiltinUsersSid,
		windows.WinAuthenticatedUserSid,
	} {
		t.Run(fmt.Sprint(sidType), func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "secret")
			if err := os.WriteFile(path, []byte("secret"), 0o600); err != nil {
				t.Fatal(err)
			}
			user, err := windows.GetCurrentProcessToken().GetTokenUser()
			if err != nil {
				t.Fatal(err)
			}
			broad, err := windows.CreateWellKnownSid(sidType)
			if err != nil {
				t.Fatal(err)
			}
			sd, err := windows.SecurityDescriptorFromString("D:P(A;;FA;;;" + user.User.Sid.String() +
				")(A;;FA;;;" + broad.String() + ")")
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
				t.Fatal("broad-principal ACE was accepted")
			}
		})
	}
}
