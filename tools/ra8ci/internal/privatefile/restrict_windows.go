//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import (
	"fmt"
	"os"

	"golang.org/x/sys/windows"
)

func restrictFile(file *os.File) error {
	if file == nil {
		return os.ErrInvalid
	}
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return fmt.Errorf("read current user SID: %w", err)
	}
	system, err := windows.CreateWellKnownSid(windows.WinLocalSystemSid)
	if err != nil {
		return fmt.Errorf("create SYSTEM SID: %w", err)
	}
	admins, err := windows.CreateWellKnownSid(windows.WinBuiltinAdministratorsSid)
	if err != nil {
		return fmt.Errorf("create Administrators SID: %w", err)
	}
	sddl := fmt.Sprintf("D:P(A;;FA;;;%s)(A;;FA;;;%s)(A;;FA;;;%s)",
		user.User.Sid.String(), system.String(), admins.String())
	sd, err := windows.SecurityDescriptorFromString(sddl)
	if err != nil {
		return fmt.Errorf("build private-file DACL: %w", err)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("read private-file DACL: %w", err)
	}
	if err := windows.SetNamedSecurityInfo(file.Name(), windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION|
			windows.PROTECTED_DACL_SECURITY_INFORMATION,
		user.User.Sid, nil, dacl, nil); err != nil {
		return fmt.Errorf("apply private-file DACL: %w", err)
	}
	return checkFile(file)
}
