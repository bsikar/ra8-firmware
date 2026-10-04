// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testprivatefile

import (
	"fmt"

	"golang.org/x/sys/windows"
)

func ownerOnly(path string) error {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return err
	}
	owner := user.User.Sid.String()
	sd, err := windows.SecurityDescriptorFromString("D:P(A;;FA;;;" + owner + ")")
	if err != nil {
		return err
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return err
	}
	return windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION|
			windows.PROTECTED_DACL_SECURITY_INFORMATION,
		user.User.Sid, nil, dacl, nil)
}

func denyDirectoryRead(path string) error {
	return denyDirectoryAccess(path, windows.FILE_LIST_DIRECTORY)
}

func denyDirectoryCreate(path string) error {
	// Directory FILE_WRITE_DATA and FILE_APPEND_DATA are FILE_ADD_FILE and
	// FILE_ADD_SUBDIRECTORY respectively.
	const addFile = 0x00000002
	const addSubdirectory = 0x00000004
	return denyDirectoryAccess(path, addFile|addSubdirectory)
}

func denyDirectoryAccess(path string, access uint32) error {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return err
	}
	sid := user.User.Sid.String()
	sd, err := windows.SecurityDescriptorFromString(fmt.Sprintf("D:P(D;;0x%x;;;%s)(A;;FA;;;%s)", access, sid, sid))
	if err != nil {
		return err
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return err
	}
	return windows.SetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION,
		nil, nil, dacl, nil)
}

func restoreDirectory(path string) error { return ownerOnly(path) }
