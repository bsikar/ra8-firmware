// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilpolicy

// DeclaredTimeout reads hil.conf as key=value text and deliberately does not
// evaluate shell syntax. The bench's own runner sources the same file with a
// shell, so the two readers do not have to agree about everything, but they
// do have to agree about whether the app declared a timeout at all.
//
// The reader took its answer from an exact match on the key: anything else
// was not a HIL_TIMEOUT_S line and the scan moved on. That is right for
// another variable and wrong for the same one written in a spelling this
// reader does not read. `export HIL_TIMEOUT_S=180` is the ordinary way a
// config meant for sourcing states a value; the shell sets 180 and this
// reader returns found=false, so the app that asked for three minutes gets
// the 30s default and its observe step is cut off mid-run and reported timed
// out. `HIL_TIMEOUT_S+=60` is the same silence.
//
// This is the failure declared_present.go already argued about from the other
// side: an app that declares no HIL timeout and an app whose declaration
// cannot be read are different answers, and every other refusal in this
// reader fails closed. A key it cannot interpret is an unreadable
// declaration, not an absence, so it is refused with the line named rather
// than skipped.
//
// The boundary is what keeps this off other variables. RA8_HIL_TIMEOUT_S and
// HIL_TIMEOUT_SEC are different names and are still skipped in silence,
// because the token has to stand alone: what sits either side of it must not
// be a character a variable name is made of.
func keyNamesTheTimeout(key string) bool {
	const name = "HIL_TIMEOUT_S"
	for index := 0; index+len(name) <= len(key); index++ {
		if key[index:index+len(name)] != name {
			continue
		}
		if index > 0 && isNameByte(key[index-1]) {
			continue
		}
		if after := index + len(name); after < len(key) && isNameByte(key[after]) {
			continue
		}
		return true
	}
	return false
}

// isNameByte reports whether b can appear inside a shell variable name, which
// is what decides whether a match is the whole name or part of a longer one.
func isNameByte(b byte) bool {
	return b == '_' || (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')
}
