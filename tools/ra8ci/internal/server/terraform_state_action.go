// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"net/http"
	"slices"
	"strings"
)

// The Terraform HTTP state backend speaks two methods HTTP does not define.
// They are named here so the handler's switch, the Allow header and the
// audited action all read the same list.
const (
	terraformLockMethod   = "LOCK"
	terraformUnlockMethod = "UNLOCK"
)

// terraformStateActionPrefix scopes the audited action to this endpoint.
const terraformStateActionPrefix = "terraform_state."

// terraformStateMethods is what this endpoint implements, in the order the
// Allow header offers them, and the only method names that may be written to
// the audit trail.
var terraformStateMethods = []string{
	http.MethodGet,
	http.MethodPost,
	http.MethodDelete,
	terraformLockMethod,
	terraformUnlockMethod,
}

// terraformStateAllow answers an unsupported method with what is accepted.
var terraformStateAllow = strings.Join(terraformStateMethods, ", ")

// terraformStateAction names the audited action for a refused Terraform state
// request.
//
// Because Terraform's backend needs LOCK and UNLOCK, this is the one route
// registered without a method, so the method arrives exactly as the caller
// wrote it. net/http admits any RFC 9110 token there, and a token may be
// punctuation (!#$%&'*+-.^_`|~) and may run to the whole request-line budget,
// so "UNLOCK!#$%" and two thousand letters both reach the handler intact.
//
// A denial is written before the request is understood, so the action it
// records has to be text the server chose, the rule certificateActor already
// keeps for the actor. Only a method this backend implements names itself;
// anything else is recorded as unsupported rather than quoted.
func terraformStateAction(method string) string {
	if slices.Contains(terraformStateMethods, method) {
		return terraformStateActionPrefix + strings.ToLower(method)
	}
	return terraformStateActionPrefix + "unsupported"
}
