// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import "os"

// confirmSignalFile holds the handle a signal was read from to the file whose
// properties were judged before it was opened.
//
// This package opens three files whose properties it judges first, and until
// now only two of them confirmed what they actually opened. LoadProfile
// re-stats its handle and calls os.SameFile before reading a byte;
// LoadVerifierFile does the same and re-checks size and permissions on top.
// SysfsReader.ReadSignal judged the resolved path with os.Stat, checked that
// it was a regular file inside the root, and then opened the path again and
// read whatever was there. One rule, three doors, one of them silent, and the
// silent one is the door reading the values that decide whether a board is
// neutral.
//
// The window is small and on a real sysfs it is empty, because those files are
// kernel-owned. It is not empty for the fixture roots this reader is built to
// take: SysRoot is configurable, and a root under a writable directory can
// have a leaf replaced between the judgement and the open. The value that
// comes back is then attributed to a file this reader checked and never read,
// and it is signed into a neutral receipt as an observation of the fixture.
//
// The pre-open judgement stays where it is rather than being replaced by an
// open-then-judge: opening a FIFO with no writer, or a device node, blocks,
// and refusing those before the open is the reason os.Stat runs first.
func confirmSignalFile(judged os.FileInfo, file *os.File) error {
	if judged == nil || file == nil {
		return ErrInvalidProfile
	}
	opened, err := file.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(judged, opened) {
		return ErrInvalidProfile
	}
	return nil
}
