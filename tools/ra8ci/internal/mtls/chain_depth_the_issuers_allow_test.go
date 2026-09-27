package mtls

import (
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"errors"
	"strings"
	"testing"
)

// authorityAtDepth is an intermediate that states how many certificate
// authorities may sit below it, the way a corporate profile constrains one.
// A negative depth leaves the field unstated, which is the common case.
func authorityAtDepth(name string, depth int) *x509.Certificate {
	authority := chainAuthority()
	authority.Subject = pkix.Name{CommonName: name}
	if depth >= 0 {
		authority.MaxPathLen = depth
		authority.MaxPathLenZero = depth == 0
	}
	return authority
}

// tieredChain mints a real path under the given authorities, leaf-first, so
// the depth rule is read against a chain that satisfies every rule before it.
func tieredChain(t *testing.T, authorities ...*x509.Certificate) tls.Certificate {
	t.Helper()
	return presentingLeaf(t, clientTemplate(), authorities...)
}

func TestAChainInsideTheDepthItsRootAllowsIsAccepted(t *testing.T) {
	for name, identity := range map[string]tls.Certificate{
		"one intermediate under a root allowing one": tieredChain(t,
			authorityAtDepth("ra8ci-intermediate", -1),
			authorityAtDepth("ra8ci-root", 1)),
		"one intermediate under a root allowing two": tieredChain(t,
			authorityAtDepth("ra8ci-intermediate", -1),
			authorityAtDepth("ra8ci-root", 2)),
		"an intermediate that may sign leaves only": tieredChain(t,
			authorityAtDepth("ra8ci-intermediate", 0)),
		"two tiers under a root allowing two": tieredChain(t,
			authorityAtDepth("ra8ci-signing", -1),
			authorityAtDepth("ra8ci-intermediate", 1),
			authorityAtDepth("ra8ci-root", 2)),
	} {
		if err := ValidateClientIdentity(identity, testNow); err != nil {
			t.Fatalf("%s: a chain within the stated depth was refused: %v", name, err)
		}
	}
}

// The field's absence is not a limit of zero. A chain under authorities that
// state nothing has to keep working, or this rule locks out every deployment
// whose profile never carried the extension.
func TestAnUnconstrainedChainIsAccepted(t *testing.T) {
	identity := tieredChain(t,
		authorityAtDepth("ra8ci-intermediate", -1),
		authorityAtDepth("ra8ci-root", -1))
	if err := checkChainDepthTheIssuersAllow(identity, "client"); err != nil {
		t.Fatalf("an unconstrained chain was refused: %v", err)
	}
}

func TestARootThatForbidsASecondTierIsRefused(t *testing.T) {
	identity := tieredChain(t,
		authorityAtDepth("ra8ci-intermediate", -1),
		authorityAtDepth("ra8ci-root", 0))
	err := ValidateClientIdentity(identity, testNow)
	if err == nil {
		t.Fatalf("a chain deeper than its root allows was accepted")
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not ErrIdentity: %v", err)
	}
	// The fix is to drop a tier or reissue the constraint, so the refusal
	// carries both numbers and the position to look at.
	for _, want := range []string{
		"position 2 of the presented client chain",
		"allows 0 certificate authorities below it",
		"the chain sends 1",
		"ra8ci-root",
	} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal does not say %q: %v", want, err)
		}
	}
}

// A constraint broken further up is the same mistake one certificate along,
// and the message has to move with it.
func TestTheRefusalNamesTheLinkThatStatedTheLimit(t *testing.T) {
	identity := tieredChain(t,
		authorityAtDepth("ra8ci-signing", -1),
		authorityAtDepth("ra8ci-intermediate", -1),
		authorityAtDepth("ra8ci-root", 1))
	err := checkChainDepthTheIssuersAllow(identity, "client")
	if err == nil || !strings.Contains(err.Error(), "position 3 of the presented client chain") {
		t.Fatalf("expected a refusal naming position 3, got %v", err)
	}
	if !strings.Contains(err.Error(), "allows 1 certificate authorities below it") ||
		!strings.Contains(err.Error(), "the chain sends 2") {
		t.Fatalf("the refusal does not carry both counts: %v", err)
	}
}

// The nearest link is the one an operator should read first, so a chain that
// breaks two constraints is refused by the lower of them.
func TestTheNearestBrokenConstraintIsTheOneReported(t *testing.T) {
	identity := tieredChain(t,
		authorityAtDepth("ra8ci-signing", -1),
		authorityAtDepth("ra8ci-intermediate", 0),
		authorityAtDepth("ra8ci-root", 0))
	err := checkChainDepthTheIssuersAllow(identity, "client")
	if err == nil || !strings.Contains(err.Error(), "position 2 of the presented client chain") {
		t.Fatalf("expected the refusal to name position 2, got %v", err)
	}
	if !strings.Contains(err.Error(), "ra8ci-intermediate") {
		t.Fatalf("the refusal does not name the nearest constrained link: %v", err)
	}
}

func TestALeafPresentedAloneHasNoDepthToJudge(t *testing.T) {
	identity := issue(t, clientTemplate())
	if err := checkChainDepthTheIssuersAllow(identity, "client"); err != nil {
		t.Fatalf("a leaf alone was refused: %v", err)
	}
	if err := checkChainDepthTheIssuersAllow(tls.Certificate{}, "client"); err != nil {
		t.Fatalf("an empty key pair was refused: %v", err)
	}
}

func TestUnparseableBytesAreRefusedNamingTheirPosition(t *testing.T) {
	identity := tieredChain(t, authorityAtDepth("ra8ci-intermediate", -1))
	identity.Certificate = append(identity.Certificate, []byte{0x30, 0x00})
	err := checkChainDepthTheIssuersAllow(identity, "client")
	if err == nil || !strings.Contains(err.Error(), "position 2 of the presented client chain") {
		t.Fatalf("expected a parse refusal naming position 2, got %v", err)
	}
}

// The listener presents a chain too, and it is walked by the same verifier.
func TestTheServerChainIsHeldToTheSameDepthRule(t *testing.T) {
	identity := presentingLeaf(t, serverTemplate(),
		authorityAtDepth("ra8ci-intermediate", -1),
		authorityAtDepth("ra8ci-root", 0))
	err := ValidateServerIdentity(identity, testNow)
	if err == nil || !strings.Contains(err.Error(), "presented server chain") {
		t.Fatalf("expected a server-side depth refusal, got %v", err)
	}
	if !errors.Is(err, ErrIdentity) {
		t.Fatalf("refusal is not ErrIdentity: %v", err)
	}
}

// Every other refusal in this package names the certificate and never the key.
func TestTheDepthRefusalNamesTheSubjectAndFingerprint(t *testing.T) {
	identity := tieredChain(t,
		authorityAtDepth("ra8ci-intermediate", -1),
		authorityAtDepth("ra8ci-root", 0))
	err := checkChainDepthTheIssuersAllow(identity, "client")
	if err == nil {
		t.Fatalf("expected a refusal")
	}
	root, parseErr := x509.ParseCertificate(identity.Certificate[2])
	if parseErr != nil {
		t.Fatalf("parse the root: %v", parseErr)
	}
	if !strings.Contains(err.Error(), Fingerprint(root)) {
		t.Fatalf("the refusal does not carry the fingerprint: %v", err)
	}
	if !strings.Contains(err.Error(), "subject ") {
		t.Fatalf("the refusal does not name the subject: %v", err)
	}
	if strings.Contains(strings.ToLower(err.Error()), "private key") {
		t.Fatalf("the refusal talks about the key: %v", err)
	}
}

// The whole point is to say here what the far end would say opaquely, so the
// rule is held against the verifier this process actually links against: the
// chain it refuses must fail verification, and the chain it accepts must pass.
func TestTheFarEndRefusesExactlyTheDepthThisRuleRefuses(t *testing.T) {
	for name, depth := range map[string]int{"forbidden": 0, "allowed": 1} {
		identity := tieredChain(t,
			authorityAtDepth("ra8ci-intermediate", -1),
			authorityAtDepth("ra8ci-root", depth))
		refused := checkChainDepthTheIssuersAllow(identity, "client") != nil
		leaf, err := x509.ParseCertificate(identity.Certificate[0])
		if err != nil {
			t.Fatalf("%s: parse the leaf: %v", name, err)
		}
		intermediate, err := x509.ParseCertificate(identity.Certificate[1])
		if err != nil {
			t.Fatalf("%s: parse the intermediate: %v", name, err)
		}
		root, err := x509.ParseCertificate(identity.Certificate[2])
		if err != nil {
			t.Fatalf("%s: parse the root: %v", name, err)
		}
		roots := x509.NewCertPool()
		roots.AddCert(root)
		intermediates := x509.NewCertPool()
		intermediates.AddCert(intermediate)
		_, verifyErr := leaf.Verify(x509.VerifyOptions{
			Roots:         roots,
			Intermediates: intermediates,
			CurrentTime:   testNow,
			KeyUsages:     []x509.ExtKeyUsage{x509.ExtKeyUsageAny},
		})
		if refused != (verifyErr != nil) {
			t.Fatalf("%s: this rule refused=%v, the verifier answered %v", name, refused, verifyErr)
		}
	}
}
