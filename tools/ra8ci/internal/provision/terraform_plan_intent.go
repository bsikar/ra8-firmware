// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"crypto/subtle"
	"errors"
	"fmt"
)

// requireApprovedPlan holds the bytes an apply is about to execute to the
// digest that was recorded as the apply intent.
//
// Plan returns a digest and says in its own doc comment that the caller must
// persist it and consume an apply intent before applying. The runner does
// exactly that: it writes PlanSHA256 into the plan evidence row and passes the
// same digest to BeginRunnerVMTerraformApply, which is the one-way gate that
// makes an apply legal. Nothing then held the file at that path to the digest,
// so the ledger recorded approval for one set of bytes and Terraform executed
// whatever was sitting at the path when it ran.
//
// The two are separated by two round trips to PostgreSQL, which is real
// wall-clock time, and the path is derivable: the workspace is named after the
// reservation and the operation directory after the operation ID, both of
// which appear in the ledger. The plan file is the whole approval. A saved
// plan already carries its resolved resource addresses, so a substituted one
// does not have to look anything like the approved change.
//
// This is the file's own stated contract, enforced. It is a separate
// question from requirePrivateFileInside, which asks whether the path is a
// private, bounded, regular file inside the reservation workspace; a
// substituted plan written by the workspace owner passes that check and always
// would.
func requireApprovedPlan(planFile, expectedDigest string) error {
	if !terraformSHA256Pattern.MatchString(expectedDigest) {
		return errors.New("Terraform apply requires the recorded plan digest")
	}
	actual, err := fileSHA256(planFile, maxTerraformPlanBytes)
	if err != nil {
		return fmt.Errorf("read Terraform saved plan digest: %w", err)
	}
	if subtle.ConstantTimeCompare([]byte(actual), []byte(expectedDigest)) != 1 {
		return errors.New("Terraform saved plan differs from the approved apply intent")
	}
	return nil
}
