// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"net/url"
)

// SyncPath is the one endpoint this client posts finished local evidence to.
const SyncPath = "/v1/local-runs/sync"

// syncEndpoint turns a configured base URL into the address the outbox is
// uploaded to, refusing anything that is not a bare HTTPS origin.
//
// The rules are the ones this package has always applied: HTTPS, a host, no
// credentials, no query, no fragment and no path, so the operator names an
// ORIGIN and this client names the path. What changes is which value the
// endpoint is built from. The checks were answered by the PARSED url and the
// endpoint was built by concatenating the RAW string, and those are not the
// same value: a parse reports what a URL MEANS, and a raw string that means a
// bare origin can still carry syntax that changes what a later concatenation
// means.
//
// Two shapes got through. A trailing "?" parses with RawQuery "" (the
// emptiness is held in ForceQuery, which nothing read), so "https://host?"
// passed every check and concatenated to "https://host?/v1/local-runs/sync",
// where the path this client meant to name is the QUERY of the origin root. A
// trailing "#" parses with Fragment "" and concatenated to
// "https://host#/v1/local-runs/sync", where the path is a FRAGMENT, which is
// not sent to a server at all.
//
// Both POST the outbox to the origin root instead of the sync endpoint, and
// the failure is quiet in the direction that matters. This client already
// refuses to follow a redirect so the outbox cannot be sent to another origin,
// and holds the receipt to the local ID and payload digest it sent. None of
// that is reached: the request goes to the right origin with the right body,
// and whatever answers the root decides what happens next. A root answering
// 200 with a well-formed receipt retires the record; a root answering anything
// else stops the whole sweep with an HTTP status against an endpoint nobody
// configured, and the operator is reading a path they did not write.
//
// So the endpoint is built from the parsed URL, which is the value the checks
// were answered about, and Path is SET rather than appended. That is what
// makes the two shapes above harmless rather than refused: neither means
// anything about where to post, so neither should decide it. There is no raw
// syntax left between what was checked and what is sent.
func syncEndpoint(baseURL string) (string, error) {
	parsed, err := url.Parse(baseURL)
	if err != nil || parsed.Scheme != "https" || parsed.Host == "" || parsed.User != nil ||
		parsed.Opaque != "" || parsed.RawQuery != "" || parsed.Fragment != "" ||
		parsed.RawFragment != "" || parsed.Path != "" || parsed.RawPath != "" {
		return "", errors.New("offline sync server must be an HTTPS origin")
	}
	endpoint := *parsed
	endpoint.ForceQuery = false
	endpoint.Path = SyncPath
	endpoint.RawPath = ""
	return endpoint.String(), nil
}
