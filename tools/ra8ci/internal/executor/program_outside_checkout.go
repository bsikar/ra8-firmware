// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import "fmt"

// resolveTaskProgram has two branches and only one of them asked where the
// program came from. A step naming a relative path is resolved from the
// verified checkout and then held to it: symlinks are followed and the target
// must still be inside, so a reviewed script cannot point at something the
// tree does not contain. A step naming an absolute path, or a bare name found
// on PATH, went straight to exec.LookPath and whatever came back was run.
//
// That is the wrong asymmetry. A relative program is checkout content and has
// to stay checkout content. An absolute or PATH-resolved program is the
// opposite claim: it is a system tool the runner image provides, reviewed by
// the image rather than by the manifest. A name in that branch that resolves
// back INTO the checkout is neither. It is a binary the working tree carries,
// running with the reviewed task's identity, and nothing in the manifest ever
// described it: the catalog reviews a bash step by its script PATH, not by the
// bytes of a program, so a binary in the tree passes review as a file nobody
// read.
//
// cleanEnvironment already makes exactly this refusal from the other side. A
// PATH entry inside the checkout is ErrUnsafeEnvironment ("PATH includes
// checkout"), and so is HOME, TMPDIR, GOCACHE or any other path value pointing
// in there. The reason is the same one: the checkout is the thing under test,
// so it may not also be the thing that supplies the tools doing the testing.
// Two ways past that door stayed open. A step could name the absolute path
// outright, and a PATH directory outside the checkout could hold a symlink
// whose target is inside it, which the PATH check does not see because it
// resolves the directory and not the entries within it.
//
// So the resolved program, after symlinks, must sit outside the verified
// checkout. It is the mirror of the relative branch's rule rather than a new
// idea, and it is deliberately about the RESOLVED path: a system tool that is
// genuinely a system tool is untouched, and the two shapes that reach into the
// tree are refused with the error cleanEnvironment already uses for them.
//
// Nothing in the reviewed catalog can hit this. ValidateStepDispatch refuses a
// program containing a separator outright and admits only bash and the ra8ci:
// tools, so a reviewed manifest has no absolute program to offer; what this
// closes is the path a task takes when it reaches the executor some other way,
// which is the same reason resolveTaskProgram checks the relative branch at
// all.
func checkProgramIsOutsideTheCheckout(root, resolved string) error {
	within, err := isWithin(root, resolved)
	if err != nil {
		return err
	}
	if within {
		return fmt.Errorf("%w: task program resolves inside the verified checkout: %s",
			ErrUnsafeEnvironment, resolved)
	}
	return nil
}
