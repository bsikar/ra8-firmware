// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package mtls

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// serverTrustBundle issues a self-signed authority with the window given and
// returns its PEM. It is the client-side twin of clientAuthorityBundle, and it
// is what a client's RA8CI_SERVER_CA holds.
func serverTrustBundle(t *testing.T, name string, notBefore, notAfter time.Time) []byte {
	t.Helper()
	template := authorityTemplate(name)
	template.NotBefore = notBefore
	template.NotAfter = notAfter
	return encodePEM(t, issue(t, template))
}

func TestServerAuthoritySourceAcceptsALiveBundle(t *testing.T) {
	clock := testNow
	bundle := serverTrustBundle(t, "ra8ci-server-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	pool, usable, err := ServerAuthoritySource(bundle, at(&clock))
	if err != nil {
		t.Fatalf("a live server CA bundle was refused: %v", err)
	}
	if pool == nil {
		t.Fatal("no pool was returned for a live bundle")
	}
	if err := usable(); err != nil {
		t.Fatalf("the first request was refused: %v", err)
	}
}

func TestAnAuthorityExpiringUnderARunningClientIsRefused(t *testing.T) {
	clock := testNow
	bundle := serverTrustBundle(t, "ra8ci-server-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	_, usable, err := ServerAuthoritySource(bundle, at(&clock))
	if err != nil {
		t.Fatalf("a live server CA bundle was refused: %v", err)
	}
	if err := usable(); err != nil {
		t.Fatalf("the request before expiry was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	err = usable()
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("a lapsed trust file was still accepted on a later poll: %v", err)
	}
	if !strings.Contains(err.Error(), "server CA bundle") {
		t.Fatalf("the refusal does not name the file to go and look at: %v", err)
	}
}

// The whole point of the refusal is that it points at this host's trust file
// rather than at the server's certificate, which is where the TLS error points
// and where the operator would otherwise go.
func TestTheRefusalNamesThisHostsFileAndNotTheServer(t *testing.T) {
	clock := testNow
	bundle := serverTrustBundle(t, "ra8ci-server-ca", testNow.Add(-72*time.Hour), testNow.Add(-time.Hour))
	_, _, err := ServerAuthoritySource(bundle, at(&clock))
	if err == nil {
		t.Fatal("a bundle that expired before the client started was accepted")
	}
	if !strings.Contains(err.Error(), "certificate authority") || !strings.Contains(err.Error(), "server CA bundle") {
		t.Fatalf("the refusal does not say which file ran out: %v", err)
	}
}

// Each end names its own file, so an operator reading one refusal knows
// whether to open the listener's client CA or their own server CA.
func TestBothEndsNameTheirOwnTrustFile(t *testing.T) {
	clock := testNow
	window := func(name string) []byte {
		return serverTrustBundle(t, name, testNow.Add(-time.Hour), testNow.Add(time.Hour))
	}
	_, serverSide, err := ServerAuthoritySource(window("ra8ci-server-ca"), at(&clock))
	if err != nil {
		t.Fatalf("a live server bundle was refused: %v", err)
	}
	_, clientSide, err := ClientAuthoritySource(window("ra8ci-client-ca"), at(&clock))
	if err != nil {
		t.Fatalf("a live client bundle was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	serverErr, clientErr := serverSide(), clientSide()
	if serverErr == nil || clientErr == nil {
		t.Fatalf("a lapsed bundle was still accepted: %v / %v", serverErr, clientErr)
	}
	if !strings.Contains(serverErr.Error(), "server CA bundle") || strings.Contains(serverErr.Error(), "client CA bundle") {
		t.Fatalf("the client's refusal does not name the client's own trust file: %v", serverErr)
	}
	if !strings.Contains(clientErr.Error(), "client CA bundle") || strings.Contains(clientErr.Error(), "server CA bundle") {
		t.Fatalf("the listener's refusal does not name the listener's own trust file: %v", clientErr)
	}
}

func TestARotatingBundleKeepsTheClientPolling(t *testing.T) {
	clock := testNow
	retiring := serverTrustBundle(t, "ra8ci-server-ca-old", testNow.Add(-72*time.Hour), testNow.Add(time.Hour))
	live := serverTrustBundle(t, "ra8ci-server-ca-new", testNow.Add(-time.Hour), testNow.Add(72*time.Hour))
	_, usable, err := ServerAuthoritySource(append(retiring, live...), at(&clock))
	if err != nil {
		t.Fatalf("a rotating bundle was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	if err := usable(); err != nil {
		t.Fatalf("a bundle whose retiring authority expired was refused: %v", err)
	}
}

// The permanent properties of the file are decided once, at construction, and
// never asked again: parseAuthorityBundle owns them and the re-check reads the
// parsed authorities.
func TestTheFilesPermanentPropertiesAreRefusedAtConstruction(t *testing.T) {
	clock := testNow
	for _, c := range []struct {
		name   string
		bundle []byte
		says   string
	}{
		{"empty", nil, "bundle is empty"},
		{"no certificate", []byte("-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n"), "holds no certificate"},
		{"an end-entity certificate", encodePEM(t, issue(t, clientTemplate())), "is not a certificate authority"},
		{"an authority that may not sign", encodePEM(t, issue(t, nonSigningAuthority("ra8ci-server-ca-nosign"))), "may not sign certificates"},
	} {
		t.Run(c.name, func(t *testing.T) {
			pool, usable, err := ServerAuthoritySource(c.bundle, at(&clock))
			if err == nil {
				t.Fatalf("%s was accepted as a trust file", c.name)
			}
			if !errors.Is(err, ErrIdentity) {
				t.Fatalf("refusal is not classifiable as an identity problem: %v", err)
			}
			if !strings.Contains(err.Error(), c.says) {
				t.Fatalf("refusal does not say what is wrong: %v", err)
			}
			if pool != nil || usable != nil {
				t.Fatal("a refused bundle handed back something to poll with")
			}
		})
	}
}

// The door and the re-ask are one rule, not two: checkAuthoritiesUsable is
// called from both, and this fails if a later edit ever restates the window in
// one of them.
func TestTheReAskAppliesExactlyTheWindowTheDoorApplies(t *testing.T) {
	opens := testNow.Add(-time.Hour)
	closes := testNow.Add(time.Hour)
	for _, offset := range []time.Duration{
		-2 * time.Hour, -time.Hour, -time.Minute, 0, time.Minute, time.Hour, 2 * time.Hour,
	} {
		instant := testNow.Add(offset)
		clock := instant
		bundle := serverTrustBundle(t, "ra8ci-server-ca", opens, closes)
		_, usable, doorErr := ServerAuthoritySource(bundle, at(&clock))
		if doorErr != nil {
			// The door refused, so there is no re-ask to cross it with.
			// What is pinned here is that the door refused exactly when
			// the window says it should.
			if !instant.Before(opens) && instant.Before(closes) {
				t.Fatalf("the door refused a bundle inside its window at %s: %v", instant, doorErr)
			}
			continue
		}
		if instant.Before(opens) || !instant.Before(closes) {
			t.Fatalf("the door accepted a bundle outside its window at %s", instant)
		}
		if err := usable(); err != nil {
			t.Fatalf("the re-ask refused at the same instant the door accepted (%s): %v", instant, err)
		}
	}
}

// The re-ask reads the clock every time rather than the instant the client was
// built, which is the whole reason it exists.
func TestTheReAskReadsTheClockEachTime(t *testing.T) {
	clock := testNow
	bundle := serverTrustBundle(t, "ra8ci-server-ca", testNow.Add(-time.Hour), testNow.Add(time.Hour))
	_, usable, err := ServerAuthoritySource(bundle, at(&clock))
	if err != nil {
		t.Fatalf("a live bundle was refused: %v", err)
	}
	clock = testNow.Add(2 * time.Hour)
	if err := usable(); err == nil {
		t.Fatal("the re-ask answered from the instant the client was built")
	}
	clock = testNow
	if err := usable(); err != nil {
		t.Fatalf("the re-ask did not read the clock back: %v", err)
	}
}

// A nil clock is time.Now, the same reading ClientAuthoritySource takes, so a
// caller that has no clock to hand is not a caller with no check.
func TestANilClockMeansTimeNow(t *testing.T) {
	now := time.Now()
	bundle := serverTrustBundle(t, "ra8ci-server-ca", now.Add(-time.Hour), now.Add(time.Hour))
	_, usable, err := ServerAuthoritySource(bundle, nil)
	if err != nil {
		t.Fatalf("a live bundle was refused under the default clock: %v", err)
	}
	if err := usable(); err != nil {
		t.Fatalf("the re-ask was refused under the default clock: %v", err)
	}
}

// The pool is the one the bundle stated. Nothing about holding the authorities
// open for a re-ask changes what the TLS stack verifies against.
func TestThePoolIsTheBundleThatWasRead(t *testing.T) {
	clock := testNow
	firstPEM, first := serverAuthorityPEM(t, serverAuthorityTemplate("ra8ci-server-ca-a", testNow.Add(-time.Hour), testNow.Add(time.Hour)))
	secondPEM, second := serverAuthorityPEM(t, serverAuthorityTemplate("ra8ci-server-ca-b", testNow.Add(-time.Hour), testNow.Add(time.Hour)))
	pool, usable, err := ServerAuthoritySource(append(firstPEM, secondPEM...), at(&clock))
	if err != nil {
		t.Fatalf("a two-authority bundle was refused: %v", err)
	}
	if !pool.Equal(poolOf(first, second)) {
		t.Fatal("the returned pool does not hold the authorities the bundle stated")
	}
	if err := usable(); err != nil {
		t.Fatalf("a live two-authority bundle was refused on the first ask: %v", err)
	}
}
