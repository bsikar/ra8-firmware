// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
	"golang.org/x/sys/windows"
)

func TestALeafKeyWithAnEveryoneACEIsRefused(t *testing.T) {
	now := testNow
	certPEM, keyPEM := clientKeyPairPEM(t, now.Add(-time.Hour), now.Add(time.Hour))
	certPath, keyPath := writeKeyPair(t, certPEM, keyPEM)
	if err := testprivatefile.OwnerOnly(keyPath); err != nil {
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
	if err := windows.SetNamedSecurityInfo(keyPath, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION,
		nil, nil, dacl, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadClientIdentity(certPath, keyPath, now); !errors.Is(err, ErrIdentity) {
		t.Fatalf("a key with an Everyone allow ACE was accepted: %v", err)
	}
}
