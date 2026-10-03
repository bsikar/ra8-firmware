// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import (
	"fmt"
	"os"
	"unsafe"

	"golang.org/x/sys/windows"
)

func check(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat private file %s: %w", path, err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("private file %s is not a regular file", path)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read private file DACL: %w", err)
	}
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return fmt.Errorf("read current user SID: %w", err)
	}
	owner, _, err := sd.Owner()
	if err != nil {
		return fmt.Errorf("read private file owner SID %s: %w", path, err)
	}
	if owner == nil || !windows.EqualSid(owner, user.User.Sid) {
		return fmt.Errorf("private file %s is not owned by the current user", path)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("read private file DACL %s: %w", path, err)
	}
	if dacl == nil {
		return fmt.Errorf("private file %s has no restrictive DACL", path)
	}
	ownerAllowed := false
	for index := uint32(0); index < uint32(dacl.AceCount); index++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err := windows.GetAce(dacl, index, &ace); err != nil {
			return fmt.Errorf("read private file DACL entry: %w", err)
		}
		if ace == nil || ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE {
			return fmt.Errorf("private file %s has an unsupported DACL entry", path)
		}
		aceSID := (*windows.SID)(unsafe.Pointer(&ace.SidStart))
		if !windows.EqualSid(user.User.Sid, aceSID) {
			return fmt.Errorf("private file %s DACL grants access beyond its owner", path)
		}
		ownerAllowed = true
	}
	if !ownerAllowed {
		return fmt.Errorf("private file %s DACL does not grant access to its owner", path)
	}
	return nil
}
