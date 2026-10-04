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
	return checkPrivateDACL(path, sd)
}

func checkFile(file *os.File) error {
	if file == nil {
		return fmt.Errorf("private file handle is nil")
	}
	info, err := file.Stat()
	if err != nil {
		return fmt.Errorf("stat private file: %w", err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("private file %s is not a regular file", file.Name())
	}
	sd, err := windows.GetSecurityInfo(windows.Handle(file.Fd()), windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read private file DACL: %w", err)
	}
	return checkPrivateDACL(file.Name(), sd)
}

func checkDirectory(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat private directory %s: %w", path, err)
	}
	if !info.IsDir() {
		return fmt.Errorf("private path %s is not a directory", path)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read private directory DACL: %w", err)
	}
	return checkPrivateDACL(path, sd)
}

func checkDirectoryNoUntrustedWrite(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat protected directory %s: %w", path, err)
	}
	if !info.IsDir() {
		return fmt.Errorf("protected path %s is not a directory", path)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read protected directory DACL: %w", err)
	}
	return checkWriteDACL(path, sd)
}

func checkPrivateDACL(path string, sd *windows.SECURITY_DESCRIPTOR) error {
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
	allowedSIDs := []*windows.SID{user.User.Sid}
	for _, sidType := range []windows.WELL_KNOWN_SID_TYPE{
		windows.WinLocalSystemSid,
		windows.WinBuiltinAdministratorsSid,
	} {
		sid, err := windows.CreateWellKnownSid(sidType)
		if err != nil {
			return fmt.Errorf("create allowed private-file SID: %w", err)
		}
		allowedSIDs = append(allowedSIDs, sid)
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
		allowed := false
		for _, allowedSID := range allowedSIDs {
			if windows.EqualSid(allowedSID, aceSID) {
				allowed = true
				break
			}
		}
		if !allowed {
			return fmt.Errorf("private file %s DACL grants access beyond its owner", path)
		}
		if windows.EqualSid(user.User.Sid, aceSID) {
			ownerAllowed = true
		}
	}
	if !ownerAllowed {
		return fmt.Errorf("private file %s DACL does not grant access to its owner", path)
	}
	return nil
}

func checkNoUntrustedWrite(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat protected file %s: %w", path, err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("protected file %s is not a regular file", path)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read protected file DACL: %w", err)
	}
	return checkWriteDACL(path, sd)
}

func checkFileNoUntrustedWrite(file *os.File) error {
	if file == nil {
		return fmt.Errorf("protected file handle is nil")
	}
	info, err := file.Stat()
	if err != nil {
		return fmt.Errorf("stat protected file: %w", err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("protected file %s is not a regular file", file.Name())
	}
	sd, err := windows.GetSecurityInfo(windows.Handle(file.Fd()), windows.SE_FILE_OBJECT,
		windows.OWNER_SECURITY_INFORMATION|windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return fmt.Errorf("read protected file DACL: %w", err)
	}
	return checkWriteDACL(file.Name(), sd)
}

func checkWriteDACL(path string, sd *windows.SECURITY_DESCRIPTOR) error {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return fmt.Errorf("read current user SID: %w", err)
	}
	owner, _, err := sd.Owner()
	if err != nil || owner == nil || !windows.EqualSid(owner, user.User.Sid) {
		return fmt.Errorf("protected file %s is not owned by the current user", path)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return fmt.Errorf("read protected file DACL %s: %w", path, err)
	}
	if dacl == nil {
		return fmt.Errorf("protected file %s has no restrictive DACL", path)
	}
	trustedSIDs := []*windows.SID{user.User.Sid}
	for _, sidType := range []windows.WELL_KNOWN_SID_TYPE{
		windows.WinLocalSystemSid,
		windows.WinBuiltinAdministratorsSid,
	} {
		sid, err := windows.CreateWellKnownSid(sidType)
		if err != nil {
			return fmt.Errorf("create trusted protected-file SID: %w", err)
		}
		trustedSIDs = append(trustedSIDs, sid)
	}
	writeMask := windows.ACCESS_MASK(windows.FILE_WRITE_DATA | windows.FILE_APPEND_DATA | windows.FILE_WRITE_EA |
		windows.FILE_WRITE_ATTRIBUTES | windows.DELETE | windows.WRITE_DAC | windows.WRITE_OWNER |
		windows.GENERIC_WRITE | windows.GENERIC_ALL)
	for index := uint32(0); index < uint32(dacl.AceCount); index++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err := windows.GetAce(dacl, index, &ace); err != nil {
			return fmt.Errorf("read protected file DACL entry: %w", err)
		}
		if ace == nil || ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE {
			return fmt.Errorf("protected file %s has an unsupported DACL entry", path)
		}
		if ace.Mask&writeMask == 0 {
			continue
		}
		aceSID := (*windows.SID)(unsafe.Pointer(&ace.SidStart))
		trusted := false
		for _, sid := range trustedSIDs {
			if windows.EqualSid(sid, aceSID) {
				trusted = true
				break
			}
		}
		if !trusted {
			return fmt.Errorf("protected file %s DACL grants untrusted write access", path)
		}
	}
	return nil
}
