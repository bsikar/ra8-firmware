//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"fmt"

	"golang.org/x/sys/windows"
)

func grantBroadAccess(path string) error {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return fmt.Errorf("read current user SID: %w", err)
	}
	sd, err := windows.SecurityDescriptorFromString(fmt.Sprintf(
		"D:P(A;;FA;;;%s)(A;;FA;;;WD)", user.User.Sid.String()))
	if err != nil {
		return fmt.Errorf("build broad test DACL: %w", err)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("read broad test DACL: %w", err)
	}
	if err := windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION|
			windows.PROTECTED_DACL_SECURITY_INFORMATION,
		user.User.Sid, nil, dacl, nil); err != nil {
		return fmt.Errorf("apply broad test DACL: %w", err)
	}
	return nil
}
