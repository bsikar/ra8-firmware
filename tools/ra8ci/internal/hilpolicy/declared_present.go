// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// An app that declares no HIL timeout and an app whose declaration cannot be
// read are different answers, and DeclaredTimeout owes its caller the
// difference. Every other refusal in that reader fails closed: a config
// escaping the approved root, one that will not open, one declaring a value
// out of bounds, a duplicate key. The absent arm was the one door that failed
// open, because filepath.EvalSymlinks reports a DANGLING link with the same
// os.ErrNotExist it reports for a name that was never there. A hil.conf
// pointing at a target that has gone, or an app directory that is itself a
// dangling link, then read as "this app declares nothing" and the bench ran
// under the 30s default while its own configuration said otherwise. The
// declared bound exists because some apps need longer than 30s, so the cost
// of that silence is an observe step cut off mid-run and reported timed out.
//
// The rule is the one os.Lstat answers: a name that EXISTS without resolving
// is a declaration this reader cannot read, and it is refused. A name that is
// not there is an absence, the ordinary case, and still found=false.

// ErrUnresolvableConfig means a HIL configuration is present under a name
// that does not resolve. It is exported so a caller can tell an unreadable
// declaration from a missing one without matching on message text.
var ErrUnresolvableConfig = errors.New("HIL config exists but does not resolve")

// configIsAbsent reports whether an app's hil.conf is genuinely not there,
// given that resolving it already failed with os.ErrNotExist.
//
// Two names can carry that failure: the file itself and the app directory
// holding it, since a dangling link at either answers the same way. The
// directory is the one that has to be asked twice, because a directory that
// is simply there and resolves says nothing about the missing file inside it.
func configIsAbsent(directory, file string) (bool, error) {
	fileThere, err := nameExists(file)
	if err != nil {
		return false, err
	}
	if fileThere {
		return false, nil
	}
	directoryThere, err := nameExists(directory)
	if err != nil {
		return false, err
	}
	if !directoryThere {
		return true, nil
	}
	_, err = filepath.EvalSymlinks(directory)
	return err == nil, nil
}

// nameExists reports whether name is an entry in its parent directory,
// without following it. A dangling symlink exists; a name that was never
// written does not.
func nameExists(name string) (bool, error) {
	if _, err := os.Lstat(name); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return false, nil
		}
		return false, fmt.Errorf("inspect HIL config %s: %w", name, err)
	}
	return true, nil
}
