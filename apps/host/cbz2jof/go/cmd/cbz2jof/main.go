// Copyright (c) 2026 Brighton Sikarskie
// SPDX-License-Identifier: MIT

// Command cbz2jof converts a CBZ archive into JOF page atlases, one worker
// invocation per image entry.
package main

import (
	"os"

	"cbz2jof"
)

func main() {
	selfExe, err := os.Executable()
	if err != nil || selfExe == "" {
		selfExe = os.Args[0]
	}
	os.Exit(cbz2jof.Run(os.Args[1:], os.Getenv, selfExe, cbz2jof.ProductionCommandRunner))
}
