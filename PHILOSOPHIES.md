<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# Philosophies

## What this codebase actually is, underneath its name

This codebase looks, at first read, like the firmware for one specific
microcontroller. Nearly every file wears the same short namespace prefix, taken from the
part the project was born on, and a newcomer skimming the tree would reasonably conclude they are looking at a single-chip
project and nothing else. That conclusion is wrong, and the gap between how the
tree *looks* and how it is *built* is the reason this document exists.

Stated plainly: **this is architecturally a chip-agnostic library platform that
happens to have been born on one microcontroller.** The shared prefix is a
namespace, not a coupling. Its whole job is to keep the project's own symbols
from colliding with the vendored ones--my filesystem's mount call must not
clash with ThreadX's `tx_thread_create()` or stb's `stbi_load()`. It says nothing
at all about whether the code behind the name touches hardware, and most of it
does not. The strongest proof of that is a fact about the prefix itself: the
entire namespace can be renamed wholesale without moving a single architectural
boundary, because the name was never where the coupling lived. A rename is a
cosmetic sweep, not a port.

This is not an aspiration or a marketing claim; it is a structural property of
the tree. Point any honest audit at the question "which libraries actually touch
a hardware register?" and the answer is lopsided in a way that does not depend on
the exact count on any given day: the register coupling is concentrated almost
entirely in a single hardware-facing library--the Hardware Abstraction Layer--
while the overwhelming majority of the project's libraries reference no register
at all. A sensor driver depends only on error-handling, logging, and validation
utilities with no hardware in them. A reflowable-text engine depends only on
graphics, glyph, and memory-arena code, every piece of it pure logic. And a large
share of the codebase compiles and runs unchanged on an ordinary development
host, with no silicon of any kind present--which is the operational definition
of not being tied to a microcontroller. Code that runs on your laptop cannot, by
construction, be chip-bound.

So the prefix is a historical accident wearing the costume of a constraint. This
document is about the architecture underneath the costume: the three layers the
tree really has, the intellectual traditions that shape them, and the one forcing
function--safety certification--that binds an otherwise eclectic set of
choices into a single, defensible philosophy. It is written deliberately in terms
of those durable layers and principles rather than the current file names, so
that it stays true when the names change.

---

## The three layers

Every file in this tree is one of three things. Learn to see which, and the whole
architecture snaps into focus. The three layers are *roles*, not directories--
the current names and paths used below are illustrations of each role, not the
definition of it.

### Layer 1--Agnostic logic

Pure C that solves a problem over an *interface* and never names a peripheral.
This is the reflowable-text engine, the EPUB and comic-book readers, the graphics
primitives, the filesystem, the flash-translation layer, the memory subsystem,
the archive and image decoders, and the sensor drivers written against a bus
abstraction. (Such libraries carry the project's namespace prefix, incidental to what they are: a reflow engine, an e-book parser, a graphics library, a filesystem.) These libraries
would move to a completely different microcontroller--or to a Linux box--with
a recompile and nothing more. They are the crown jewels, and they are the part of
the tree least aware of what chip they are on.

### Layer 2--The port / HAL seam

Code that programs *this* silicon's registers and boots *this* core: the Hardware
Abstraction Layer (the hand-written register headers and the drivers that program
them), the TrustZone Non-Secure-Callable veneers, the per-application boot
substrate (each app's vector table, system-init, and linker script), and the thin
adapter shims that graft vendored code onto this hardware. This is the layer that
gets rewritten when you change chips. It is supposed to be small relative to
Layer 1, and it is: essentially one library out of the whole set carries the
overwhelming majority of the register-level code.

### Layer 3--Vendored SOUP plus glue

Third-party "software of unknown provenance" that was written elsewhere to be
freestanding, vendored into the tree, and adapted to this hardware by a small
piece of glue: an RTOS, a flash wear-levelling layer, networking and
cryptography stacks, compression and image codecs, USB and Bluetooth stacks, a
Wi-Fi co-processor link, machine-learning runtimes for the NPU-bearing part.
Each arrives as a large, chip-blind core plus a *tiny* shim.
The ThreadX port is the cleanest illustration: a mere handful of glue files
against a ThreadX core of many thousands, its heart a single hand-tuned assembly
file that does nothing but reconcile ThreadX's boot assumptions with this
project's own vector table and SysTick programming.

### The line that actually matters

Here is the subtlety that dissolves the whole "vendored code is portable but my
own code is chip-specific" confusion. The distinction that matters is **not**
vendored-versus-mine. It is **logic versus mechanism**.

ThreadX-the-core is logic; its boot-init assembly shim is mechanism. And the
first-party code splits along exactly the same seam: the reflow engine is logic;
the graphics-LCD-controller driver inside the HAL is mechanism. The vendored world
simply *advertises* its split--upstream core in one place, port glue in another
--while the first-party code has historically hidden the identical split behind a
single uniform prefix. That concealment is a cosmetic fact, not an architectural
one, which is precisely why the naming can be reorganized to expose the split
without moving the boundary an inch. ThreadX did the favour of sorting itself into
"the portable part" and "the part that had to be written for this chip"; the
first-party libraries are sorted the same way, whether or not the names yet say
so.

That is the entire insight. Once you stop asking "is this file vendored?" and
start asking "is this file logic or mechanism?", the tree reveals itself as what
it is: a large body of portable logic, a thin skin of chip mechanism, and a pile
of well-chosen freestanding libraries assembled by exactly the same recipe.

---

## The intellectual lineage

I did not invent any of this. What follows is an honest accounting of the
traditions this codebase draws from--who articulated each idea, why it applies
here, where you can see it in the tree, and, crucially, what *enforces* it so the
principle cannot quietly decay into a slogan. Read together, they are not a single
doctrine. They are a synthesis, and the last section of this part names that
synthesis plainly.

### Library, not framework--the Hollywood Principle

There is a precise, load-bearing distinction between a library and a framework,
and it is the one most people feel but cannot name. **A library is code you call.
A framework is code that calls you.** The industry's own name for the latter is
the Hollywood Principle--the casting-couch brush-off repurposed as an
architectural slogan:

> "Don't call us, we'll call you."--the Hollywood Principle (software folklore
> of the 1980s framework literature)

The technical term is Inversion of Control: the framework owns the flow, the entry
point, the lifecycle, and hands *you* the small blanks to fill in.

The idea received its canonical academic statement from Ralph Johnson and Brian
Foote in their 1988 paper ["Designing Reusable
Classes"](https://www.laputan.org/drc/drc.html). Johnson--a professor at the
University of Illinois at Urbana-Champaign, and later one of the four authors of
the 1994 "Gang of Four" book *Design Patterns*--observed that a mature framework
does not merely offer services to be called; it dictates the architecture, and the
application code is the variable part it plugs in and invokes. Foote, his student,
would go on to coin "Big Ball of Mud" for what a codebase becomes when that
discipline is absent. The distinction was then sharpened for a generation of
working programmers by Martin Fowler, whose 2004 essay ["Inversion of Control
Containers and the Dependency Injection
Pattern"](https://martinfowler.com/articles/injection.html) and companion [bliki
entry](https://martinfowler.com/bliki/InversionOfControl.html) drew the bright
line: with a library you remain in charge and call it when you choose; with a
framework the control is inverted, and the framework calls your code at the
extension points it defines.

A framework owns `main()`. It owns the build system, the configuration language,
the initialization order, the device model, the memory model. You conform to its
shapes; you write code that slots into the holes it leaves. That is not a
criticism--it is a bargain, and often a good one. But it is a bargain in which
you trade ownership for breadth.

This codebase takes the other side of that bargain, deliberately and everywhere.
The application owns `main()`--it lives at the outermost architectural ring, in
each app's own directory. The application owns the vector table and the linker
script, at the innermost boot ring. Nothing in the library layer owns the entry
point, the boot sequence, or the build. The libraries are *called*: an application
calls into the HAL, and the HAL never reaches up to call the application. The
architectural-ring system this project documents is precisely a statement of that
asymmetry--crossing *down* the rings (an app calling a driver) is normal;
crossing *up* (a driver calling application code) is a layering violation the
tooling rejects. Inversion of Control is, in this tree, an inverted-control
*prohibition*.

*What enforces it:* a layering gate that rejects any driver reaching upward into
application code, and the simple structural fact that `main()` lives in the
application, never in a library.

### The Unix philosophy--do one thing well

The Unix philosophy has a specific author. Malcolm Douglas McIlroy--a
mathematician who ran the Computing Techniques Research Department at Bell Labs
from 1965 to 1986, the man who proposed the Unix pipe and wrote the original
`spell`, `diff`, and `join`--summarized the design ethos in the foreword to the
July-August 1978 *Bell System Technical Journal* special issue on Unix. His
formulation is the sentence every working programmer eventually rediscovers:

> "Make each program do one thing well. To do a new job, build afresh rather than
> complicate old programs by adding new features."--M. D. McIlroy, 1978

The pipe was McIlroy's idea; Ken Thompson and Dennis Ritchie, who had built Unix
at Bell Labs from 1969, implemented it in 1973, and overnight the operating system
became a composition engine--small tools joined end to end through a universal
text interface. Peter H. Salus later condensed McIlroy's ethos, in his 1994
history *A Quarter Century of UNIX*, into the three-line version that now travels
as "the Unix philosophy": write programs that do one thing well, write programs to
work together, write programs to handle text streams because that is a universal
interface. Brian Kernighan and Rob Pike elaborated it into a craft in ["The Unix
Programming
Environment"](https://en.wikipedia.org/wiki/The_Unix_Programming_Environment)
(1984), as Kernighan and P. J. Plauger had earlier in *The Elements of Programming
Style* (1974) and *Software Tools* (1976); Eric S. Raymond gave it its book-length
modern treatment in ["The Art of Unix
Programming"](http://www.catb.org/~esr/writings/taoup/) (2003). The nuance the
tradition insists on is composition over accretion--McIlroy's own most famous
demonstration was reviewing a many-page literate program Donald Knuth had written
to count word frequencies, and answering it with a six-command shell pipeline that
did the same job.

This codebase is the Unix philosophy applied to firmware modules instead of
command-line tools. The rule "one module equals one purpose, one function equals
one action" is written into the project's design guidance and lived in the
structure: a hard file-size ceiling forces any source file that outgrows it to be
split along a real responsibility boundary, not chopped arbitrarily.
The I/O layer is the composition principle made concrete--not one fat I/O
god-object but a family of small, single-purpose interfaces: a stream interface, a
block-device interface, an SPI-bus interface, an I2C-bus interface, each doing one
thing, each substitutable, each composable with the others. A filesystem composed
onto a block device composed onto a caching layer composed onto an SD card is a
Unix pipeline that happens to move sectors instead of bytes.

*What enforces it:* the file-size gate that splits oversized translation units,
and the interface-segregation discipline visible in the I/O layer's many small
headers rather than one monolithic I/O header.

### Mechanism, not policy--the X11 maxim

The separation of mechanism from policy is older than the maxim it is usually
quoted as. It was articulated as an operating-system design principle by William
Wulf and his colleagues at Carnegie Mellon in the
[Hydra](https://en.wikipedia.org/wiki/Hydra_(operating_system)) kernel project
around 1974: a kernel should provide the raw *mechanisms*--the primitives--and
leave *policy*, the decisions about how to use them, to the layers above, so that
one mechanism can serve many policies. The principle found its most-quoted home a
decade later in the X Window System, designed by Robert W. Scheifler and Jim
Gettys at MIT's Project Athena and described in their 1986 [*ACM Transactions on
Graphics* paper](https://dl.acm.org/doi/10.1145/22949.24053). Their design rule
became the canonical statement of the idea:

> "Provide mechanism rather than policy. In particular, place user interface
> policy in the clients' hands."--Scheifler and Gettys, the X Window System

X therefore knows how to draw a rectangle and route a mouse click, and steadfastly
refuses to decide what a window should look like or how it should behave--which
is exactly why one protocol outlived a long parade of wildly different desktops
built on top of it. (Gettys, characteristically, went on decades later to diagnose
"bufferbloat" in the internet's queues; the instinct to find the layer where a
problem actually lives is the same one.)

The Hardware Abstraction Layer here is mechanism in exactly this sense. A HAL
driver knows how to make the graphics LCD controller scan a framebuffer, how to
make an SPI peripheral clock out bytes, how to arm a timer. It does not know or
care whether those bytes are an EPUB page or a sensor reading, whether the timer
is a game loop or a watchdog kick. The ring model is a mechanism-to-policy
gradient made architecture: driver-ring code supplies mechanism, application-ring
code supplies policy, and the platform-abstraction layers in between--the
display, network, and USB PALs--are the negotiated boundary where mechanism is
offered up through an interface for policy to consume. The display PAL knows how to
push pixels; the reader application decides what pixels mean.

*What enforces it:* the ring tags in every driver's file header, and the
platform-abstraction interfaces that force policy to enter through a defined seam
rather than reach into a driver's guts.

### Freestanding C--the stb, SQLite, and musl tradition

There is a quiet tradition in C of writing code that depends on almost nothing: no
operating system, no heap it does not manage itself, no libc beyond a documented
handful of functions, configuration by macro and callback rather than by ambient
global state. Its purest expression is Sean Barrett's
[`stb`](https://github.com/nothings/stb) libraries. Barrett--who signs his work
"nothings" and spent his career as a game programmer, including at Looking Glass
Studios--writes single-file, public-domain C libraries you drop into any project
on any platform, where they simply work. The collection describes itself in one
line:

> "single-file public domain (or MIT licensed) libraries for C/C++."--stb

The same instinct at larger scale gives us SQLite, created by D. Richard Hipp in
2000 and dedicated to the public domain. SQLite ships as an "amalgamation"--the
[entire database engine concatenated into one `sqlite3.c`
file](https://www.sqlite.org/amalgamation.html)--and is one of the most widely
deployed pieces of software on Earth precisely because it assumes so little about
its host. It is also the pointed proof that freestanding and safety-critical are
allies rather than opposites: SQLite's [test
harness](https://www.sqlite.org/testing.html) reaches 100 percent branch coverage
and was built in part to satisfy DO-178B, the avionics standard that is the direct
ancestor of this project's own DO-178C target. Rich Felker's
[musl](https://musl.libc.org/) libc (2011) and the [Lua](https://www.lua.org/)
language--created by Roberto Ierusalimschy, Luiz Henrique de Figueiredo, and
Waldemar Celes at PUC-Rio in Brazil in 1993--are cut from the same cloth: small,
freestanding, readable, portable by construction, embeddable anywhere a C compiler
reaches.

This is the recipe by which every vendored component in Layer 3 arrives--and it
is no coincidence that stb is literally one of them. The pattern is always "a large
freestanding core plus a thin per-target glue," and the ratio between the two is
the measure of the design's health: a port of a handful of files against a
vendored core of many thousands. The largest port in the tree runs to a few dozen
files only because a whole co-processor protocol lives inside it. The smaller the
port relative to the core, the more of the value is portable. And the same recipe
governs the first-party code: the off-target host-build path is nothing but a
standing declaration that the logic layers are freestanding enough to run on a
development host--which a large body of the code already does, every day, in the
test suite.

*What enforces it:* the off-target host build and the host unit-test suite, which
will not compile a "portable" library that has quietly grown a dependency on the
metal.

### Worse is better--Gabriel, and the suckless instinct

Richard P. Gabriel is a rare figure: a Stanford computer-science PhD who founded
Lucid, Inc. (maker of Lucid Common Lisp, and the Emacs fork that became XEmacs),
who also holds an MFA in poetry and has written on software as a craft. In a 1990
keynote that grew into the essay ["Lisp: Good News, Bad News, How to Win
Big"](https://www.dreamsongs.com/WorseIsBetter.html), he wrestled with a painful
question: why had Unix and C--technically inferior, in his community's eyes, to
the elegant Lisp-machine world--won so decisively? His answer was the ["Rise of
Worse Is Better"](https://www.dreamsongs.com/RiseOfWorseIsBetter.html) section,
which set the "New Jersey style" (Bell Labs, Unix, C--prize simplicity of
*implementation* above all) against "the MIT/Stanford approach" (prize
correctness, completeness, and consistency--"the right thing"). The
counterintuitive thesis:

> "It is better to get half of the right thing available so that it spreads like a
> virus. Once people are hooked on it, take the time to improve it to 90% of the
> right thing."--Richard P. Gabriel

The worse-is-better system is smaller, simpler, and more implementation-honest; it
does less on day one, but you can hold it in your head, read all of it, and own it
completely. Gabriel was honest enough to stay ambivalent about his own thesis--he
later argued the other side under the pseudonym Nickieben Bourbaki--and that
tension is the whole point: the value is not in "worse," it is in *ownable*. The
[suckless](https://suckless.org/philosophy/) community, founded by Anselm R. Garbe
around tools like `dwm` and `st`, carried the temperament to its logical end, even
configuring programs by editing a `config.h` and recompiling rather than shipping a
runtime configuration language. Their motto is the whole argument compressed:
software that "sucks less."

Choosing to hand-write the HAL against the register map, when the silicon vendor
ships a complete, generated, vendor-supported software package that would have done
it automatically, is a worse-is-better decision in its clearest form. The vendor
package is more capable on day one. The hand-written HAL is more *mine*: every
register write carries a citation to the Hardware User's Manual, every line sits
inside a scope I can reason about, and nothing in the critical path is code I have
not read. On day one that is a worse deal. Over the life of a safety-critical
project it is the only deal that makes sense, for reasons the forcing-function
section below will make unavoidable.

*What enforces it:* the citation policy--every register access must be preceded
by a Hardware User's Manual reference, checked by the citation gates--which is
only possible *because* the HAL is hand-written and owned.

### Own your core--vendoring as positive NIH

["Not Invented Here"](http://www.catb.org/jargon/html/N/Not-Invented-Here.html) is
usually an insult--the Jargon File defines it as the tendency to reject anything
one did not build oneself. But there is a disciplined, positive version of the same
instinct, and Joel Spolsky--co-founder of Fog Creek Software and Stack Overflow
--made the case for it in his 2001 essay ["In Defense of Not-Invented-Here
Syndrome"](https://www.joelonsoftware.com/2001/10/14/in-defense-of-not-invented-here-syndrome/).
His rule is not "build everything"; it is a rule about where the line falls:

> "If it's a core business function--do it yourself, no matter what."--Joel
> Spolsky

The safety world has its own vocabulary for the other side of that line. "SOUP"--
Software Of Unknown Provenance--is a term of art from IEC 62304, the
medical-device software standard, for code you depend on but did not develop under
your own controlled process; the entire discipline is to minimize it, bound it, and
justify what remains. Married to the modern practice of *hermetic* dependencies--
pinning and checking vendored code into your own tree rather than resolving it from
the network at build time, in the spirit of Nix and Bazel--the doctrine becomes:
draw a bright line around the core you must reason about, build and own that, and
vendor the rest pinned, checked in, and documented.

This codebase vendors aggressively and owns deliberately. The third-party stacks
live in a dedicated vendored tree, checked in rather than pulled from a package
manager at build time, each accompanied by a written per-component justification.
What is vendored is the stuff whose reimplementation would be foolish: a certified
RTOS, a TLS stack, a cryptography library, image and compression codecs. What is
owned is the layer where correctness is mine to prove: the HAL, the boot path, the
interfaces, the logic. The vendored core is not trusted blindly--it is bounded,
documented, and kept minimal, and components are retired when they stop earning
their place. This is NIH turned from a vice into a boundary-drawing discipline.

*What enforces it:* the per-component SOUP justification requirement, the
certification-scope documents that must account for every vendored line, and the
hermetic in-tree vendoring that makes "what are we actually depending on"
answerable by reading the tree.

### Ports and Adapters--Hexagonal Architecture

Alistair Cockburn is a methodologist rather than a systems programmer--one of the
seventeen authors of the 2001 Agile Manifesto, a PhD who wrote *Writing Effective
Use Cases* and *Agile Software Development*--and around 2005 he wrote up, on [his
own site](https://alistair.cockburn.us/hexagonal-architecture/), a pattern he had
watched be rediscovered many times: Hexagonal Architecture, which he later
preferred to call **Ports and Adapters** (the hexagon shape means nothing in
particular; he chose it only because it leaves room to draw several ports around
one core). The application core is surrounded by *ports*--abstract interfaces
stated in the core's own terms--and each port is filled by an *adapter* that
speaks to some concrete outside world. His statement of intent is worth quoting
whole:

> "Allow an application to equally be driven by users, programs, automated test or
> batch scripts, and to be developed and tested in isolation from its eventual
> run-time devices and databases."--Alistair Cockburn

The core never names a database, a network, a screen; it names a port, and adapters
make the ports real. The payoff is that you can swap the outside world--a real
database for an in-memory fake, a network socket for a test double--without the
core noticing. The idea sits in a family with Robert C. Martin's Dependency
Inversion Principle (the "D" in SOLID) and Jeffrey Palermo's later "Onion
Architecture"; all three are the same instinct--point the dependencies inward, at
interfaces the core owns, never outward at concrete machinery.

This is not an analogy for what the project's I/O subsystem does; it is a
by-the-book implementation of it. The block-device interface is a port: a single
logical-block-address vtable--read, write, erase, query-capabilities, sync--and
its own documentation states the goal exactly, that the layers above it
(filesystem, cache, VFS) never name a peripheral. The adapters are the backends: a
RAM disk, a native SD host controller, SD-over-SPI, OSPI NOR flash, on-chip
non-volatile RAM, an SDRAM ramdisk--one adapter per medium, each in its own file.
The domain core is the filesystem, and a single bridging call adapts any backend
into the filesystem's own backend interface, so the same FAT implementation mounts
on *any* medium without knowing which. The identical shape repeats for the bus
facades: the SPI-bus and I2C-bus interfaces are ports, and the chip's twin SPI
peripherals and twin I2C peripherals are drop-in adapters behind them.

And this is where the sensor question answers itself. What separates a "generic"
driver from a "chip-tied" one? A driver is generic when it talks to a *port* and
chip-tied when it talks to a *register*. A sensor driver talks to an I2C-bus port;
it neither knows nor cares whether the bytes came from one vendor's I2C peripheral,
another's, or a Linux `/dev/i2c-1`. The adapter is chip-bound; the driver on top of
it is not. The line is not fuzzy once you know where to look: it runs exactly along
the port.

*What enforces it:* the invariant that logic libraries include no register header
at all--today an observed fact, and the natural thing to freeze into a gate (see
the closing section)--together with the discipline that new storage media arrive
as adapters behind the existing port rather than as new code paths cut through the
filesystem.

### The synthesis--"freestanding hexagonal C"

None of the seven traditions above is mine, and their combination is not a new
"-ism" that deserves a manifesto. What this codebase is, honestly stated, is a
*personal synthesis* of five well-trodden lineages: the Unix philosophy's
composition of small single-purpose units; the library-not-framework insistence on
owning the entry point and the flow; the freestanding-C tradition of a portable
core plus a thin per-target port; hexagonal architecture's discipline of ports and
adapters; and the own-your-core approach to vendoring. If I had to compress the
combination into two words, it would be **freestanding hexagonal C**: portable
logic cores that speak only to ports, filled by thin per-chip adapters that I own.

There is no shame in this being a synthesis rather than an invention. It is a
strength. Every one of these ideas has decades of production evidence behind it;
none of them is speculative; and the fact that they compose so cleanly--that
Unix's "one thing well" and Cockburn's ports and Barrett's freestanding-C all point
the same direction--is not luck. They point the same direction because they are
all, underneath, the same conviction stated at different altitudes: keep the thing
you must reason about small, sharp-edged, and yours. The next section is about the
one external pressure that turns that conviction from a matter of taste into a
matter of necessity.

---

## A vocabulary interlude--HAL, BSP, FSP, and libs

Four words get used loosely in embedded work, and the looseness is the source of
real confusion. They form a ladder, lowest to highest.

- **BSP--Board Support Package.** Everything true about *this particular board,
  chip package, and pinout*: the vector table, the clock-tree bring-up, the linker
  memory map, the pin multiplexing. In this tree that is the innermost boot ring
  (each app's vector table, system-init, and linker script) plus the board-support
  libraries that hold the board pin maps. BSP answers "what board am I on?"

- **HAL--Hardware Abstraction Layer.** How to *drive this chip's peripherals*:
  register layouts and the drivers that program them, exposed through a clean C API
 --the register and driver rings, gathered in the one hardware-facing library.
  HAL answers "how do I work this silicon?"

- **FSP--Flexible Software Package.** The Renesas vendor SDK. Critically, this is
  not a peer of the HAL--it is a *superset* that bundles a BSP, a HAL, middleware,
  RTOS glue, and a graphical configuration generator into one vendor-owned stack. It
  is precisely the whole thing this project declined: the guidance is unambiguous
  that no FSP code lives in this tree, that FSP is reference material only, and that
  the HAL is hand-written. FSP answers "what if the vendor built the whole lower
  stack for me?"--and the deliberate answer here is "then I would not own it."

- **libs--the library layer.** Everything *above* the HAL: the core utilities and
  the agnostic logic of Layer 1. Chip-blind by construction. This is the part that
  would survive a change of silicon untouched.

So the ladder is **BSP below HAL below libs**, with **FSP being a vendor's
pre-assembled bundling of BSP plus HAL plus middleware** that this project replaces
with owned, hand-written equivalents at the two lower rungs and keeps entirely its
own at the top.

---

## Why not Zephyr

The obvious question, having built all this by hand, is why not simply adopt Zephyr
--a mature, well-governed RTOS with an enormous driver tree, a configuration
system, and a real ecosystem. The answer is the library-versus-framework axis,
sharpened to a point.

Zephyr is a framework in the full Hollywood-Principle sense. It owns `main()`. It
owns the build through CMake and Kconfig. It owns the device model through
devicetree. It owns the initialization sequence, the driver API shapes, the memory
model, the RTOS. You write code that fills in Zephyr's blanks and conforms to
Zephyr's shapes. In exchange you receive breadth that is genuinely, and I want to be
fair here, extraordinary: hundreds of supported boards, thousands of drivers,
subsystems for networking and Bluetooth and storage and sensors that would each take
months to write. For a huge class of products Zephyr is exactly the right choice,
and at the far edges of the portability question this document will reach--a
Linux-class SoC, an unfamiliar board--Zephyr's breadth is a decisive advantage
that owning-your-own-stack simply cannot match.

What you give up is ownership, and for this project ownership is not a preference.
It is a requirement imposed from outside, by the safety bar. And that is the subject
of the next section, because DO-178C is the reason the entire eclectic philosophy
above stops being a matter of temperament and becomes the only coherent choice.

---

## The forcing function--safety certification as the through-line

Everything above could be dismissed as taste. A person could like small tools and
readable source and owning their core, and another person could reasonably prefer
the breadth of a framework, and there would be no fact of the matter between them.
What removes the argument is the certification target: this project aims at the
DO-178C Level B and IEC 61508 SIL 3 safety bar. Once that is on the table, the taste
hardens into necessity, and every one of the seven traditions turns out to be
pulling in the same direction for the same underlying reason.

Under DO-178C, every line of code inside the certification boundary must be
justified. Requirements trace to code; code traces to tests; compound boolean
decisions must be exercised to Modified Condition/Decision Coverage, where each
condition is shown to independently affect the outcome. This is not paperwork you
add at the end. It is a property the architecture must be built to make *possible*,
and it reframes every choice above:

- A **framework** you did not write is, under this bar, a liability rather than an
  asset. Adopting Zephyr would mean bringing its code inside your reasoning
  boundary, and you would be on the hook to justify what you did not author and do
  not fully control. The library-not-framework stance is what keeps the
  certification boundary drawn around code you own.

- **Owning your core** stops being NIH vanity and becomes the mechanism by which
  the boundary is finite. You vendor SOUP where reimplementing would be reckless,
  you document each component's justification, and you keep the vendored surface
  minimal precisely because every vendored line is a line you must account for.

- **Worse is better** becomes literally true: a hand-written HAL where every
  register access cites the Hardware User's Manual is auditable in a way a generated
  vendor package is not. The citation policy is only *possible* because the HAL is
  owned; the worse-on-day-one choice is the only certifiable one.

- **Ports and adapters** stop being an elegance and become a testing strategy. The
  block-device port and the bus facades are what let the logic be exercised on a
  host, against fake adapters, to the coverage the standard demands--the
  off-target host build and the sim-equals-hardware principle are downstream of the
  hexagonal seams. You cannot get MC/DC on a filesystem you can only run on
  hardware; you can get it on a filesystem that mounts on a RAM adapter on your
  laptop.

- **Small, single-purpose modules** are what make the coverage tractable at all. A
  thousand-line file with a dozen responsibilities is a coverage nightmare; the
  file-size gate and the single-responsibility rule exist because short, sharp
  modules are the only ones you can prove things about.

This is the through-line. The reason the philosophy coheres--the reason Unix
composition and freestanding C and hexagonal ports and owned vendoring all belong in
the same codebase without friction--is that a safety certification demands a
finite, owned, auditable, testable core, and every one of those traditions is a
different facet of building exactly that. The synthesis is not arbitrary. It is what
the forcing function selects for.

---

## Where portability is a fantasy, and why that is correct

An honest philosophy names its own limits. There are parts of this tree that are
inescapably tied to the hardware, and the mark of good architecture is not
pretending otherwise--it is putting the tie exactly where it belongs and letting
it be honest.

**TrustZone-M is Armv8-M and nothing else.** The entire TrustZone veneer layer, the
secure-boot machinery, the secure/non-secure world tagging, the
`cmse_nonsecure_entry` attributes, the Security Attribution Unit programming--all
of it is a concept that exists in the Armv8-M security extension and literally
nowhere else. There is no TrustZone on an MSP430, on an RX72N, on an Xtensa or
RISC-V ESP32, on a Cortex-A Linux SoC. On a Linux-class part the analogous concern
is served by a completely different mechanism--process isolation, TrustZone-A, a
separation kernel. Trying to abstract the veneer layer into something "portable"
would be building an interface with exactly one possible implementation, which is
not abstraction, it is decoration. The right answer is to let that library be
honestly, permanently Armv8-M-bound. That is not a defect. It is the tie living
where it belongs.

**The MPU-versus-MMU boundary is a hardware fact, not a leaky abstraction.** The MPU
driver programs an Armv8-M Memory Protection Unit--a region-based,
no-address-translation protection scheme. A Linux-class part has a full Memory
Management Unit with virtual memory, and protection there is the kernel's job, not
the application's. These are not two implementations of one interface; they are two
different machines. Portable logic never touches either, and that is why it ports;
the code that does touch them is Layer 2, and it is supposed to be rewritten per
target.

**The graphics controller is vendor intellectual property.** The graphics-LCD
driver in the HAL programs a specific Renesas IP block. An STM32's equivalent is
called LTDC and has a different register interface; a stock ESP32 has no
parallel-RGB TFT controller at all and drives displays over SPI. The *interface*
above it--the display PAL--can be portable, and should be; the driver beneath it
cannot, and should not pretend to be.

Notice the pattern in all three. The lock-in is real, it is confined to Layer 2, and
it is confined there *on purpose*. Good architecture does not abolish hardware
dependence--that is impossible, someone has to touch the metal. It concentrates the
dependence into a thin, honestly-labelled layer and keeps it out of everything
above. The fantasy is not "the HAL is portable." The fantasy would be believing you
had made TrustZone portable when all you had done was wrap it.

There is a corollary worth stating, because it is the most counterintuitive and most
valuable observation in the whole picture. As you carry this codebase to ever more
distant targets--from the Cortex-M85 to its companion Cortex-M33, to a 16-bit
MSP430, to a Linux-class edge-AI SoC--the mechanism layer does not merely change.
It progressively *evaporates*. On a Linux SoC there is an operating system, so the
RTOS is irrelevant; there is virtual memory, so the MPU code is the kernel's
problem; there is a driver model, so the HAL dissolves into device drivers you do
not write. What survives that journey, untouched, is Layer 1--the reflow engine,
the reader, the filesystem on a file-backed adapter, the graphics primitives against
a framebuffer. The further you travel, the more the chip-bound skin falls away and
the more the portable logic is *all that is left standing*--because the logic was
the only thing that was ever chip-blind to begin with. The architecture is not just
portable. It is *most* valuable exactly where portability is hardest.

---

## Keeping this document honest

A philosophy document is the one kind of document a project's tooling cannot verify,
which makes it the most likely to rot into an aspirational manifesto that describes
a codebase that no longer exists. This project's own hardest-won lesson is that a
rule nothing enforces is a rule that has already been broken somewhere.

A note on how this document is written, because it bears directly on the honesty it
demands of itself. Everything above is stated in terms of durable *layers*,
*principles*, and *enforced invariants* rather than a snapshot of the current tree
--role names instead of file paths, "concentrated in a single hardware-facing
library" instead of a line count that expires next week, "an invariant a gate keeps
true" instead of a grep result true only today. That is a deliberate choice, and it
is made precisely because the specifics move: a namespace rename, a
reorganization along architecture / chip / board lines, or a change of part will
each falsify today's prefix, today's paths, and today's counts. A philosophy
pinned to those specifics would read as false the morning after a rename, through
no change in anything actually true. Pinned instead to principles and to the
checks that guard them, it survives its own subject moving underneath it.
That is the whole trick to a document outliving the code it describes: say
the durable thing, and name the gate rather than the grep.

So this section ends not with a peroration but with a table, tying each principle to
the role it plays and the thing that keeps it true.

| Principle | Where it lives (by role) | What keeps it true |
|---|---|---|
| Library, not framework (IoC) | The application owns `main()` and boot; libraries are only ever called | A layering gate rejects any driver that calls upward into application code |
| Unix: one thing well | The I/O layer's small single-purpose interfaces | The file-size ceiling and interface-segregation in the header split |
| Mechanism, not policy | HAL drivers vs platform-abstraction seams vs application policy | Ring tags; the PAL interfaces force policy through a defined seam |
| Freestanding C + thin port | A handful of port files over a vendored core of thousands | The off-target host build and the host unit-test suite |
| Worse is better / own the HAL | A hand-written HAL, no vendor package checked in | Hardware-User's-Manual citation gates on every register access |
| Own your core / vendor hermetically | A dedicated vendored tree with per-component justifications | SOUP justification docs; certification-scope accounting |
| Ports and adapters (hexagonal) | The block-device and bus interfaces, filled by per-medium adapters | The invariant that logic libraries include no register header |

The final row is weaker than the others, and the difference is worth naming. That
the logic libraries include no register header is an *observed* fact--an audit that
comes back clean--rather than a gated one. What would make the platform claim
machine-true rather than merely true-for-now is promoting that observation into a
**gate**: a check that fails the build the instant any library on the agnostic side
of the line grows a dependency on a register header or on the HAL. The moment such a
gate exists, the three-layer model stops being a description a reader must trust and
becomes an *invariant the tree cannot violate*. That is the arc of every good idea in
this codebase: it begins as taste, hardens into discipline under the pressure of the
safety bar, and finally freezes into a check that makes the discipline impossible to
forget. This document is the taste written down.
