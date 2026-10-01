// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"io/fs"
	"os"
	"testing"
	"time"
)

// nodeWithoutKernelIdentity looks like a device node and carries no kernel
// stat behind it, which is what a synthetic or remote filesystem hands back.
type nodeWithoutKernelIdentity struct{ sys any }

func (nodeWithoutKernelIdentity) Name() string       { return "ttyACM0" }
func (nodeWithoutKernelIdentity) Size() int64        { return 0 }
func (nodeWithoutKernelIdentity) Mode() fs.FileMode  { return fs.ModeDevice | 0o660 }
func (nodeWithoutKernelIdentity) ModTime() time.Time { return time.Time{} }
func (nodeWithoutKernelIdentity) IsDir() bool        { return false }
func (node nodeWithoutKernelIdentity) Sys() any      { return node.sys }

func TestDeviceNumberRefusesANodeWithNoKernelIdentity(t *testing.T) {
	// A node whose stat the kernel does not back cannot be compared by
	// device number, and the answer is "unknown" rather than device 0,
	// which every other node would then appear to match.
	for _, node := range []os.FileInfo{
		nodeWithoutKernelIdentity{},
		nodeWithoutKernelIdentity{sys: "not a stat"},
	} {
		number, ok := deviceNumber(node)
		if ok || number != 0 {
			t.Fatalf("device = %d, ok = %v", number, ok)
		}
	}
}

func TestNumericPIDRefusesANameThatIsNotAPID(t *testing.T) {
	for _, name := range []string{"", "self", "12a", "1 2", "-1", "١٢٣"} {
		if numericPID(name) {
			t.Fatalf("%q was read as a PID", name)
		}
	}
	for _, name := range []string{"1", "4096", "0"} {
		if !numericPID(name) {
			t.Fatalf("%q was not read as a PID", name)
		}
	}
}
