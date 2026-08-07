# PowerPC 32-bit performance: the JIT question, and what actually helps

## Can we take the JIT from TheMiningTeamYT/ppc32-jdk?

No — not in any form that reaches this port. The reasons are structural, not
effort-related.

**1. It is a different virtual machine.** Java-OS4 runs **JamVM 2.0.1** with the
OpenJDK 8 class library. `ppc32-jdk` is a full **HotSpot** OpenJDK 8 (8.0.265).
HotSpot's JIT compilers are not detachable components: C1 and C2 are written
against HotSpot's object model (`oop`/`Klass` layout, compressed oops), its GC
write barriers, its safepoint protocol, its deoptimisation framework, `nmethod`
/ `CodeCache`, and its exception and stack-walking conventions. None of that
exists in JamVM. "Taking the JIT" would mean **replacing the VM with HotSpot**,
not adding a compiler to the VM we have.

**2. Replacing the VM means porting HotSpot to AmigaOS.** The fork's
`hotspot/src/os_cpu` contains **`linux_ppc` only**, and `hotspot/src/os` has no
AmigaOS layer. HotSpot leans hard on POSIX behaviour this platform does not
provide: it takes SIGSEGV to implement implicit null checks, uses page
protection for safepoint polling and stack banging, and needs an mmap that
yields executable memory. clib4's signal support is limited enough that this
port had to delete JamVM's own signal shims. That is a port of a different order
of magnitude from the current one.

**3. The fork's ppc32 work is not for our hardware anyway.** Its
`cpu/ppc/vm` sources are still the stock 64-bit ones — `templateTable_ppc_64.cpp`,
`interp_masm_ppc_64.cpp`, `ppc_64.ad`; there are no `_ppc_32` files. What it does
add, in `globalDefinitions_ppc.hpp`, is **SPE** support (`__SPE__`, `SPE_ABI`,
`SPE_ATOMIC`) — the Freescale e500/e500v2 signal-processing FPU. AmigaOS 4
hardware is 603e/604e/G3/G4, 440/460 and PA6T: classic FPUs, not SPE. So even
the CPU backend would not fit.

For completeness: OpenJDK has never supported ppc32 with a JIT. The only
upstream route is **Zero**, which is interpreter-only, plus **Shark**, its
LLVM-based JIT — and Shark was removed in JDK 9, needs an LLVM build for the
target, and is tied to Zero. This fork has `share/vm/shark` (it comes with
8u) but no `cpu/zero/vm` at all, so Shark could not be built there either.

## What does help: JamVM's own engines were switched off

JamVM ships four interpreter engines. Upstream's `configure` enables the fastest
one **by default on powerpc**, including when cross-compiling. This port had all
of them off and, on top of that, compiled the VM at **`-O0`**.

| | before | after |
|---|---|---|
| VM optimisation | `-O0` | `-O2` |
| Engine | switch-based | direct-threaded + stack caching (level 2) |

The engine ladder, now selectable with `JAMVM_ENGINE` (see `src/config.h`):

| level | engine | what it removes |
|---|---|---|
| 0 | switch-based | — (what this port used to build) |
| 1 | threaded + stack caching | the C `switch` dispatch; operand-stack top moves into registers |
| 2 | + direct + prefetch | bytecode is rewritten to handler addresses on first execution, so dispatch costs no decode. Prefetch is a powerpc-only upstream default |
| 3 | + inlining | **the code-copying engine**: handler machine code is copied into a per-block buffer, so dispatch disappears entirely inside a basic block |

Level 3 is the closest thing to a JIT that is actually available here, and
PowerPC supports it: `arch/powerpc.h` already implements the two primitives it
needs — `FLUSH_CACHE` (icache sync after copying code) and `GEN_REL_JMP` (the
relative branch it patches in). `RUNTIME_RELOC_CHECKS` is on because we
cross-compile: the build machine cannot execute the ppc handlers to measure
which of them are relocatable, so the VM measures them at startup instead.

Level 3 also requires `-fno-reorder-blocks` on the interpreter translation
units, which the build scripts now pass — without it gcc is free to move handler
bodies apart or splice their shared tails, and copying them out becomes unsound.

## Level 3 miscompiles here: default is 2

Level 3 was the default briefly, and it does not work on AmigaOS 4.  It does not
crash -- it computes **wrong answers**, intermittently:

```
java.lang.OutOfMemoryError: array length 1073741824 too large
    at java.util.concurrent.ConcurrentHashMap.initTable(ConcurrentHashMap.java:2233)
    ...
    at java.util.Locale.<clinit>(Locale.java:490)
```

2^30 is `ConcurrentHashMap.MAXIMUM_CAPACITY`, reached because the constructor's

```java
long size = (long)(1.0 + (long)initialCapacity / loadFactor);
```

came out enormous instead of 22.  That is a long-to-float conversion, a float
division and a double-to-long conversion -- the floating-point handlers.  The
same binary passed a full NetTest run (13/13, TLS included) and failed on the
next, which is the signature of inlining rather than of a plain miscompile:
copying is profile-driven, so a block is only ever copied once it has run often
enough, and whether that happens depends on the run.

The likely mechanism is in how relocatability is decided.  `relocatability.c`
compiles the interpreter twice (`interp.c`, `interp2.c`) and `memcmp`s each
handler's machine code between the two; identical bytes are taken to mean the
handler contains nothing position-dependent.  But a PC-relative branch to a
target **outside** the handler encodes the same displacement in both copies --
so it passes -- and lands somewhere else entirely once the block is copied
elsewhere.  Modern gcc manufactures precisely those by merging the common tails
of different handlers, and upstream's lone `-fno-reorder-blocks` does not stop
it.  `-fno-crossjumping -fno-tree-tail-merge` are therefore added at level 3 as
a candidate fix -- untested on hardware, which is exactly why the default is 2.

Level 2 keeps computed-goto dispatch, stack caching, direct threading and
prefetching, and copies no code at all.  Silent wrong arithmetic is not
something to ship as a default; on a machine running an accounting application,
least of all.

## Status and how to bisect

All four levels build and link, and each reports the expected engine through
`java -version`. **None of them has been run on AmigaOS 4** — this was developed
and verified on the cross-build only.

`src/config.h` previously carried the comment *"Something's wrong with the
INLINING"*, so level 3 has been tried here before and failed at runtime. The
cause was never recorded, and it may well have been the missing
`-fno-reorder-blocks` or the missing `RUNTIME_RELOC_CHECKS` (a cross-compiled
build that computes relocatability at build time gets it wrong for the target).
Both are now correct, but that is a hypothesis, not a diagnosis.

So: if the VM misbehaves, step down a level rather than reverting the lot —
levels 1 and 2 involve no code copying at all and are much less likely to be
implicated:

```sh
JAMVM_ENGINE=2 make -f Makefile.local vm
```

`java -version` prints the engine in use, so the level in a given build is always
checkable:

```
Execution Engine: inline-threaded interpreter with stack-caching
```

## Not done

- **`-O3`, `-mcpu=`, AltiVec.** Left alone deliberately. `-mcpu=` would trade
  away compatibility across the AmigaOS 4 hardware range (604e through PA6T) for
  a small gain, and it is the sort of change that wants measurement on real
  hardware first.
- **`libmanagement.so`.** Missing, so `ManagementFactory.getRuntimeMXBean()`
  throws. Unrelated to speed.
