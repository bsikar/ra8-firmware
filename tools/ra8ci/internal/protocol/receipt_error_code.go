// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// reportedErrorCodes is the whole vocabulary an agent can state. The agent
// writes exactly one of these and nothing else (agent.go, terminalReceipt
// setting executor_error, log_upload_error, artifact_upload_error or
// no_step_executed), and the schema version pins it: a receipt whose code is
// not in this list is not a newer agent's code, because a new code is a change
// to the message and arrives with a version this build refuses outright.
//
// Each one names the half of the attempt that did not complete, and the plane
// stores it verbatim as the attempt's result_reason (store/dispatch.go), beside
// reasons the plane itself authors for attempts no agent finished
// (run_cancelled_before_ack, assignment_deadline_without_terminal). That column
// is the one field an operator reads to learn why an attempt ended the way it
// did, so an agent-supplied string outside the vocabulary lands there
// indistinguishable from a reason the plane wrote.
var reportedErrorCodes = map[string]bool{
	"executor_error":        true,
	"log_upload_error":      true,
	"artifact_upload_error": true,
	"no_step_executed":      true,
}

// checkErrorCodeMatchesTheReport holds a terminal receipt's error code to what
// the rest of the receipt says about the attempt. Nothing read the field at
// all: Validate judged the outcome, the exit code, the evidence flag, the
// stamps and every step, and the one field naming what went wrong travelled
// unexamined from the agent's JSON into the attempt's durable result_reason.
//
// The agent sets a code and clears EvidenceComplete from the same three errors
// and the same empty step list (agent.go: EvidenceComplete is runErr, logErr
// and artifactErr all nil with at least one step, and the code is set when one
// of those is not nil or the steps are empty). So the two fields are one
// statement made twice, and a receipt claiming complete evidence while naming
// an error is claiming both that nothing went wrong and that something did.
// The plane believes the flag: taskResultFor downgrades an unverifiable green
// on the evidence flag alone (store/dispatch.go), so the contradiction is
// resolved silently in favour of the half that suits the receipt, and the
// operator is left the code that says otherwise.
//
// no_step_executed is refused beside steps for the same reason in the other
// direction: it is the code for an attempt that ran nothing, and a receipt
// listing steps under it contradicts the only thing the code says.
//
// The rule stays one-sided everywhere else. An incomplete receipt naming no
// code is left alone, because an attempt can end without the agent reaching
// any of the four cases, and the receipt reporting that honestly is not a
// contradiction to resolve here.
func checkErrorCodeMatchesTheReport(receipt TerminalReceipt) error {
	if receipt.ErrorCode == "" {
		return nil
	}
	if !reportedErrorCodes[receipt.ErrorCode] {
		return fmt.Errorf("%w: receipt states error code %q, which no agent reports", ErrInvalid, receipt.ErrorCode)
	}
	if receipt.EvidenceComplete {
		return fmt.Errorf("%w: receipt states complete evidence and the error %s", ErrInvalid, receipt.ErrorCode)
	}
	if receipt.ErrorCode == "no_step_executed" && len(receipt.Steps) > 0 {
		return fmt.Errorf("%w: receipt states no step executed and reports %d", ErrInvalid, len(receipt.Steps))
	}
	return nil
}
