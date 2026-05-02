# SOUP Justification: lwIP

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting lwIP into this firmware as
Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: lwIP (Lightweight IP)
- **Version**: 2.2.0 (per `src/include/lwip/init.h` LWIP_VERSION_MAJOR /
  MINOR / REVISION = 2 / 2 / 0; CHANGELOG STABLE-2.2.0 marker).
- **Upstream URL**: https://savannah.nongnu.org/projects/lwip/ (mirror:
  https://git.savannah.nongnu.org/cgit/lwip.git)
- **Local path**: `libs/third_party/lwip/`

## Provenance

- **Origin**: Originally the Swedish Institute of Computer Science
  (Adam Dunkels, 2001); now a community-maintained Savannah project.
- **License**: 3-clause BSD (`COPYING`, "Copyright (c) 2001, 2002
  Swedish Institute of Computer Science. All rights reserved.").
- **How it entered our tree**: Vendored snapshot of the upstream lwIP
  STABLE-2.2.0 release tree. Upstream commit hash unknown.

## Use case in this firmware

- Alternative TCP/IP stack used by `examples/ek_ra8d2/threadx_lwip_tcp_echo`
  for cross-validation against the NetX Duo path.
- Integrity claim category: data-handling (TCP/IP framing, only used in
  the cross-check demo).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: lwIP has shipped in millions of embedded devices
  since 2001 across consumer, industrial, and automotive domains.
- **Open-source community process**: Active community on Savannah with
  documented contribution rules.
- **Bug tracker review**: Bug tracker at
  https://savannah.nongnu.org/bugs/?group=lwip and CVE database
  reviewed; no open advisories affect the build configuration we use
  (no DHCP server, no PPP, no SNMP).

## Risk mitigation

- All board-specific Ethernet/PHY plumbing is isolated in
  `libs/ra_net_pal/` (lwIP PAL variant) so the SOUP boundary is a
  single driver shim.
- Demo-only use; no safety-critical control loop runs over lwIP.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
