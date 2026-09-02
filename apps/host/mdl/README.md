<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# mdl

A host CLI that follows serialized comics -- manga, manhwa, manhua, webtoons --
and packages them into a file the e-reader firmware opens directly. It is built
from the firmware's own code behind the same dependency-inversion seams the
device uses, so every layer above the network and storage backends is already
the one the RA8 will run; only the leaf backends get swapped.

`--help` is the authority on its interface. This page is the part it cannot
tell you.

## Where the code lives

The downloader and the command line are two different things, and they sit in
two different places. This directory is the host composition root: `main()`,
the argv grammar, the libcurl backend, the host credential policy, and the one
file where a validated command line meets an application entry point.
Everything the downloader actually *does* -- fetch, state, cache, export,
verify, report, pack, and the site descriptors that drive them -- is the
portable core under `apps/shared_libs/mdl`, written over an injected transport
and a filesystem seam.

The core sits outside this product because the same core has a second build
form ahead of it, a loadable module that runs on the device; a core owned by
one form is a core the other form has to copy. The layering is one-way -- the
core never includes from a form -- and the standing proof is that the core
configures, builds and passes its own test suite with no `find_package(CURL)`
anywhere in its listfile.

## Following a series

A run reads a site descriptor, lists a series' chapters, downloads them and
packages the result. Persistent state is what makes that a library rather than
a scrape: each series directory keeps a state file recording the series
identity and metadata, per-chapter completion, and the content hash of every
page fetched. Three behaviours fall out of it:

- **Updating is incremental.** Only chapters not already recorded complete get
  fetched, so following an ongoing series is one command, repeated.
- **An interrupted run resumes.** A chapter counts as complete only once every
  page is fetched and its bytes verified, so the next run refetches exactly the
  missing pages and reproduces identical page numbering.
- **Chapters are selected by chapter NUMBER**, never by position in today's
  chapter list, so a selection stays correct when the site inserts or reorders
  chapters.

A byte-identical image already held is reused rather than refetched, across
reruns and across chapters that share it. Chapters combine by default into one
archive with continuous page numbering, so the result reads as one contiguous
book; one archive per chapter is available instead.

A series can also be searched for or browsed by name rather than by URL, and a
hit fed straight into a download. Discovery goes through the same politeness
governor and `robots.txt` gate as a download -- search is not a rate-limit
bypass -- and zero results, changed markup, and a failed request are three
distinct messages, never an empty list dressed up as success.

## Vertical-scroll webtoons need JOF

A webtoon page is a single tall strip: 5,000-12,000 px high in a real chapter.
Every comic container -- CBZ, CBT and the rest -- decodes a page as one whole
image through stb_image, which is capped at 8192 px per side so an on-device
decode fits its memory budget. A taller strip is rejected, by design rather
than by bug. JOF is the answer: it tiles the strip through the firmware's
streaming JPEG decoder, with no whole-image allocation, and the `longstrip`
engine scrolls it a viewport at a time. So a webtoon downloaded as CBZ opens
but its tall pages will not render, while the same series as JOF renders all of
them. Standard fixed-page comics are fine as CBZ. JOF is also written as
per-page files in the chapter folder rather than one archive -- the
webtoon-native form.

Past loose page images, the export formats are CBZ, CBT, gzipped CBT, EPUB3,
JOF, and the reader-native RABOOK in its chunked container. Every one is
produced by in-tree or vendored code, with no system library and no external
process, and JOF reuses the firmware's own producer, so a `.jof` the CLI writes
is byte-identical to one the RA8 produces. CBR and CBT.XZ exist as reader-side
formats but cannot be exported here.

Packaging is idempotent: only real page images are ingested, so a folder that
already holds this tool's own output re-packages cleanly instead of folding a
previous archive in as a page.

## Site descriptors

Adding a site is dropping a `.conf` in the core's `sites/` -- flat `key = value`, no
rebuild. The descriptors that ship are the qualified ones, each backed by
captured fixtures. A descriptor carries the chapter, image and discovery rules,
the politeness governor's delay bounds, language and reading direction, and
bounded metadata selectors: read a meta value, read cleaned element text, read
the value following a visible label, or supply a constant the descriptor owns.
Unknown keys, malformed selectors and contradictory bounds fail configuration
rather than being quietly ignored, and a descriptor with no discovery rules
reports that mode unavailable instead of guessing.

Sites behind a JavaScript challenge are not claimed supported.

## The rest of what it does

It verifies what it already holds, re-checking recorded content hashes and
parsing each container structurally -- archive members and their CRCs, tar
headers, gzip framing, tile tables -- and names the formats it cannot
structurally verify rather than passing them silently. It re-packages an
existing folder of images with no network at all, manages the library as a
whole, and generates a starter site descriptor for a new site. A bare artifact
URL is streamed to a sibling staging file, structurally validated, and only
then published atomically, so a failed or unsupported download never replaces
an artifact that was already good.

Exactly one mode runs per invocation, and an option with no effect in the
selected mode is a usage error rather than a silently ignored flag -- dispatch
never falls back on precedence.

## Being polite, and not being an SSRF gadget

`robots.txt` is honoured by default: fetched once per host, most-specific
`User-agent` group applied, a disallowed URL refused before any request is
made, and a declared crawl delay raises the per-host floor. The User-Agent is
truthful and held constant per session -- rotating one reads as *more* bot-like,
not less -- and carries a contact you set, so an operator can reach you instead
of banning a netblock. Backing off further is available; ignoring robots is a
loud, explicit escape hatch rather than anything a default reaches.

The libcurl backend is hardened for attacker-controlled URLs: transport pinned
to http and https, redirects refused when they change host or resolve into
loopback, private or link-local space -- the guard runs on the *resolved* peer
-- explicit TLS verification, `.netrc` and proxy environment disabled, and per
response a size and low-speed cap. A custom PEM bundle can be supplied for a
private endpoint without weakening verification. Proxying requires explicitly
opting into private address space, because libcurl's proxy callback cannot
prove the proxy's target is public.

## Why it is shaped this way

The network seam is a function-pointer vtable mirroring the on-device OTA
network interface, and every layer reaches a backend only through its
dispatchers. The composition root is the one place naming a concrete backend,
so libcurl today, the C6 link on the board, and a scripted mock in the tests
are vtable substitutions rather than relinks -- the NASA Rule 9 DIP deviation.
Return values are `ra8_err_t`, so the signatures are already device-shaped, and
the configuration and library-state structs are fixed-size with zero dynamic
allocation.

## What is not claimed

Host-qualified means the test suite proves it, including a real loopback
HTTP/HTTPS run whose fixture mints an ephemeral CA and checks that an untrusted
server fails while an explicitly supplied bundle succeeds without weakening
verification. Not claimed: CBR and CBT.XZ export, a fixed-memory
arbitrary-URL HTTPS implementation on the ESP32-C6 (ESP-IDF's stack allocates
transitively), full scrape-and-package orchestration across the two processors,
and hardware reader qualification -- the C6 path transfers a prebuilt artifact
today rather than running the whole URL-to-RABOOK workflow. Those boundaries
return an error or say unsupported; none of them pretends to work.
