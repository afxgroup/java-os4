# Changelog

All notable changes to Java-OS4. Versions are AmigaOS 4 `.lha` releases on the
[Releases page](https://github.com/derfsss/java-os4/releases). Java-OS4 is a Java 8
runtime for AmigaOS 4 (PowerPC): JamVM 2.0 + the OpenJDK 8 class library + a native
`sun.awt.amiga` AWT/Swing toolkit.

## 0.6.0 — 2026-08-09

The release where a real application runs. Networking, TLS, colour management
and the AmigaOS path model all arrived or were rebuilt; the interpreter stopped
being the slowest one JamVM offers. Then the parts that worked were made fast
enough to use: an https download went from 140 KB/s to 1.7 MB/s, and external
processes started at all.

**Added**

- **Networking** — `libnet` is a real implementation instead of a stub, so
  `java.net` works: sockets, DNS, `URLConnection`, HTTP. Previously
  `java.net.PlainSocketImpl`'s class initialiser threw
  `UnsatisfiedLinkError: initProto` and nothing network-facing could start.
- **HTTPS/TLS** — `SunEC` is built and shipped (elliptic curve), together with
  `lib/security` (the JCE jurisdiction policy jars, `java.security`, `cacerts`)
  and `lib/ext` (the JCE providers). Without the policy jars `JceSecurity`
  threw *"Cannot locate policy or framework files!"* and anything touching
  `Cipher` — including `SSLContext.getInstance()` — died in its class
  initialiser. `securerandom.source` now points at AmigaOS's `RANDOM:` device
  rather than `/dev/random`, falling back to the threaded seed generator where
  that is unavailable.
- **Colour management** — `liblcms.so` (Little CMS) plus the `lib/cmm` ICC
  profiles. `ICC_Profile` needs both; without them
  `ColorSpace.getInstance(CS_GRAY)` threw *"Can't load standard profile:
  GRAY.pf"* and took out anything doing image work.
- **A faster interpreter** — the VM was built at `-O0` with every one of JamVM's
  interpreter engines switched off, leaving the plain switch-based dispatch.
  It now builds at `-O2` with computed-goto dispatch, stack caching, direct
  threading and prefetching. `JAMVM_ENGINE=0..3` selects the level; `java
  -version` reports which is in use. See
  [`docs/ppc32-performance.md`](docs/ppc32-performance.md).
- **`:` as a classpath separator, and wildcards** — `java -cp app.jar:lib/*` now
  works. `path.separator` is `;` on AmigaOS because `:` is the volume separator,
  but `:` is what everyone types, so each one is now resolved against the
  AmigaDOS device list: `Work:` is a volume, `app.jar:` is a separator. `*` and
  `dir/*` expand to the jars in that drawer, which no AmigaOS shell does.
- **`Runtime.exec` and `ProcessBuilder`** — previously impossible and documented
  as such: clib4 has neither `fork` nor `vfork`, and OpenJDK's `UNIXProcess`
  wants both. It is now built on clib4's `spawnvpe`, which is what AmigaOS
  actually offers, with a `Platform.AMIGAOS` arm in `UNIXProcess` that replaces
  the `sigchld`-driven reaper with a bounded wait. Redirection, exit codes,
  `destroy()` and both output streams work. Examples `ExecTest` and `ExecLeak`.
- **Crypto in C, where TLS spends its time** — there is no JIT, so none of
  HotSpot's crypto intrinsics exist and every cipher ran as bytecode. The four
  methods HotSpot would intrinsify are now native: `GHASH.processBlocks`,
  `GCTR.update`, `SHA2`/`SHA5.implCompress`. AES-GCM went from **506 to 6311
  KB/s**, a factor of twelve, and https from 140 KB/s to 1.7 MB/s. `CryptoBench`
  measures it and reports call counters, not merely whether a method carries the
  native flag — see the Fixed entry below for why that distinction was expensive.
- **Faster startup** — `SecureRandom` seeded from clib4's `getentropy()` instead
  of the threaded fallback that races threads against each other to harvest
  scheduling jitter. Worth 25-30 seconds off **every** JVM start; what remains
  is ~11s of provider registration and 118 CA certificates parsed interpreted.
- **Socket defaults worth having** — `TCP_NODELAY` on, and send/receive buffers
  sized for a real link rather than left at the handler's default. Doubled plain
  http throughput on its own.
- `libbz2.so` and `libbrotli*.so` are shipped, so freetype's optional paths
  resolve instead of being left dangling.
- Examples `NetTest` (walks the java.net layers bottom-up) and `NetDownload`,
  which reports the **spread** and stall count alongside the average: a transfer
  averaging 1 MB/s by alternating 250 KB/s and 2 MB/s is not a slow transfer but
  a stalling one, and the two have different causes. That distinction is what
  identified the collector below.
- [`docs/TODO.md`](docs/TODO.md) — the known gaps, each written up from the
  diagnosis rather than the symptom, so the next person does not start over.
- Host unit tests for the path conversion and the classpath rewriter, compiled
  against the shipped headers and run by the native build, plus a sweep in
  packaging that reports any symbol a shipped `.so` needs and nothing provides.
- The JRE Lucida fonts are shipped instead of DejaVu.

**Fixed**

- **The AmigaOS path model** — `.`, `..` and trailing slashes are not what they
  are on Unix (`.` and `..` do not exist at all; a trailing `/` means the
  *parent*), and `java.io.File.normalize()` does not remove them. Every path
  carrying one silently addressed the wrong place or failed. The conversion is
  now a single source of truth with 38 host test cases, where it used to be two
  hand-kept copies.
- **The current directory** — the VM chdir'd into its own install drawer at
  startup, so relative paths an application handed to a native call resolved
  there rather than where the user launched. `java.util.zip.ZipFile` passes
  `File.getPath()` straight to `open()`, so an application's own
  `new ZipFile("App.jar")` was looked for under `JAVA:`. The boot classpath is
  java.home-absolute now and the chdir is gone.
- **`rename()`** — AmigaDOS `Rename()` will not replace an existing destination,
  and some handlers (`ENV:`) do not implement it at all, so every
  write-to-temp-then-rename failed. Now deletes the target and retries, then
  falls back to copy-then-delete-source.
- **`user.home`, `user.name`, `java.io.tmpdir`** — all unset on AmigaOS, and a
  null property renders as `"?"`, which AmigaDOS reads as a pattern character.
  They now resolve to `ENV:`, a real name, and `T:`.
- **The heap could not grow** — a fake 64MB physical-memory figure produced
  min == max == 16MB. Physical memory is now reported for real, and the startup
  mapping halves and retries rather than aborting, which is what forced the fake
  figure in the first place.
- **`-Dfoo=bar` never reached the VM** — no system property could be set without
  quoting it, on a runtime where every tuneable *is* a system property. The
  `java` launcher was an AmigaDOS script, so its command line went through
  ReadArgs, and ReadArgs treats `=` as an argument separator: `-Dfoo=bar`
  arrived as `-Dfoo` and `bar`, and since the first non-option token is the main
  class, the VM went looking for a class named after the value —
  `-Djava.security.debug=provider` → `ClassNotFoundException: provider`. That
  silently included the `sun.nio.fs.chdirAllowed` BUILDING.md tells people to
  pass, which had therefore never once arrived. `java` is now a real program
  ([`src/launcher/java.c`](src/launcher/java.c)) that hands argv to the VM as an
  array, so nothing re-parses it. Three things fell out of doing it properly: it
  no longer leaves `LD_LIBRARY_PATH` and `JAVA_HOME` set **globally** in `ENV:`
  after exiting (AmigaDOS `SetEnv` is system-wide and persistent); it returns the
  VM's own exit code, so scripts testing `$RC` get a real answer; and it locates
  the runtime by resolving its **own** directory at startup rather than assuming
  `JAVA:`, so an installation can be moved or kept in more than one place. (The
  absolute `-rpath=JAVA:Sobjs` in every shipped `.so` still assumes `JAVA:`, so
  that is not yet fully true — the launcher no longer adds to the problem.)
- **Threads could not be stopped, so exiting killed them** — `pthread_kill`
  cannot interrupt a thread running bytecode on AmigaOS, so the VM had been
  taking threads down from outside. That crashed in `pthread 6` on every clean
  exit. JamVM's cooperative safepoints are restored instead: the interpreter
  polls `suspend_pending_count` at method entry, taken branches and returns, and
  a thread leaves through `exitDetachAndDie()` on its own. This also fixed a
  latent hang nobody had hit yet — stop-the-world GC used the same mechanism, so
  it could wait forever on a thread that was never going to answer. The poll is
  declared next to the engine-variant selection, so adding an engine cannot
  silently drop it.
- **The heap never grew, and the download oscillated because of it** — separate
  from the 16MB ceiling below. `expandHeap()` is reached only after a collection
  that *still* could not satisfy the allocation which triggered it, which never
  happens with a healthy live set, so the heap stays at `min_heap` for the life
  of the VM: `min_heap` is the working size, not a starting size. The default
  was `phys_mem/64` — HotSpot's ratio for the *initial* heap, copied without the
  policy that has HotSpot raise it. TLS allocates about seven bytes per byte
  transferred, so a 77MB download collected **40 times**, once every two
  seconds, and swung between 250 KB/s and 2 MB/s. Now `phys_mem/8` capped at
  256MB: the same download collects twice, runs 44s instead of 78, and holds
  1.3-1.8 MB/s with no sample below a quarter of peak.
- **Exec left ~30 `PIPE:` handles open per run** — the DOS handles survived the
  process, so the jars could not be replaced afterwards. Three causes, found
  only by counting: the parent's own pipe ends were inherited by the child
  (clib4's `build_fd_inherit_spec` passes every descriptor without `FD_CLOEXEC`);
  the streams were drained with a blocking read that could not finish; and
  `available()` reported nonsense on a pipe, which also produced
  `OutOfMemoryError` in the process reaper. `FIONREAD` was implemented in clib4
  for the third — the patch is upstream.
- **`Runtime.exec` opened a requester for `/T:`** — the program and working
  directory were handed to DOS unconverted, so a Unix-form path arrived where an
  AmigaOS one was expected.
- **Symbols went unresolved across shipped libraries** — `GetNativePrim` in
  `libfontmanager`, `registerAmigaAwtCleanup` in `libamigaawt`. The JDK's native
  libraries expect to share one symbol namespace; they were being loaded
  privately. Now `RTLD_GLOBAL`.
- **A native method that was never called still reports as native** — three
  builds reported accelerated AES from reflection while the Java loop did all
  the work. A native binds only to libraries owned by its own class loader
  (`ClassLoader.findNative`, no fallback to the system list), and `GCTR` comes
  from the extension loader while the library was in `libjava.so`; the
  `UnsatisfiedLinkError` was then swallowed by a `catch (Throwable)`. Asking
  whether a method *is* native is not asking whether that native *runs*. The
  native lives in its own `libamigacrypto.so` now, loaded by the class that uses
  it, and failure is reported once and loudly rather than caught.
- **`libsunec.so` needed `libstdc++.so`**, which is not shipped: the AmigaOS ELF
  loader failed it with *"Unresolved symbol: __gthread_mutex_destroy"*. Linked
  statically, so those references are absent rather than merely satisfied.
- **Diagnosis quality** — a failed system class loader used to masquerade as
  `NoClassDefFoundError: <your class>` (the `-classpath` is not searched without
  it, and `-jar` hid this further); the two `OutOfMemoryError`s — heap
  exhaustion and an impossible array length — were indistinguishable. Both now
  say what actually happened.
- Ctrl-C reaches the exit; the finalizer trace no longer floods the serial line;
  shutting down no longer reports our own thread interrupt as an application
  error. The AWT open-window registry is guarded by a mutex rather than
  `Forbid`/`Permit`.
- File timestamps ran ahead of the clock because clib4 took the UTC offset from
  `locale.library` for file dates and `timezone.library` for the clock. Fixed in
  clib4; the patch is recorded in
  [`docs/clib4-amiga-filetime.patch`](docs/clib4-amiga-filetime.patch).

**Known issues**

- The inline-threaded interpreter (`JAMVM_ENGINE=3`) computes wrong
  floating-point results, intermittently, and is therefore not the default —
  see [`docs/ppc32-performance.md`](docs/ppc32-performance.md) for the evidence
  and a candidate fix.
- `libmanagement.so` is not built, so `ManagementFactory.getRuntimeMXBean()`
  throws `UnsatisfiedLinkError`.
- `file:/RANDOM:` cannot be opened through Java's `file:` URL layer, although
  `RANDOM:` is a perfectly real AmigaOS device that `open()` reads without
  complaint. Worked around by seeding from `getentropy()`.
- AES-CBC is still interpreted (`AESCrypt.encryptBlock`,
  `CipherBlockChaining.implEncrypt`). It does not currently matter — the
  negotiated GCM suites are native, and the cipher is no longer the bottleneck —
  but it would if a CBC-only peer were ever on the other end.

## 0.5.4 — 2026-06-23

**Fixed**

- **`sun.boot.class.path` separator** — the property was exposed as the VM's raw
  `:`-joined boot path (`niopatch.zip:resources.jar:rt.jar:…`), but on AmigaOS
  `path.separator` is `;` (`:` is the volume separator). OpenJDK's
  `sun.misc.Launcher.getBootstrapClassPath()` splits the property on `path.separator`,
  so the whole value was read as one bogus entry beginning `niopatch.zip:` →
  AmigaDOS popped a **"Please insert volume niopatch.zip"** requester and every
  `getBootstrapResource()` failed. That broke `sun.reflect.misc.MethodUtil`, which
  then defined `sun.reflect.misc.Trampoline` via the bootstrap loader and threw
  **"Trampoline must not be defined by the bootstrap class loader"** — first hit
  running Swing applications and the test suite (amigans.net report). The property
  is now emitted as a `;`-separated list of absolute paths anchored at `java.home`,
  so it splits correctly and every boot resource resolves. (`HelloJava` was never
  affected.) Headless apps, `java -version`, and the regression suite are unchanged.

**Added**

- `tests/regression/BootClassPathTest.java` — verifies the boot-classpath property
  splits correctly, a bootstrap resource resolves, and a `java.beans.Expression`
  reflective bounce through `MethodUtil`/`Trampoline` succeeds (fails on the
  pre-0.5.4 VM, passes on 0.5.4 and on a reference JVM).

## 0.5.3 — Amiga-1251 charset

- Fixed a `NoClassDefFoundError` for the Amiga-1251 charset at VM bootstrap.

## 0.5.2 — `java -cp`

- Fixed `java -cp` for both relative and absolute `Volume:dir` classpath entries,
  resolved against the caller's directory with no shell-cwd leak (`path.separator`
  set to `;`; each `-cp` entry rewritten to an absolute Unix-form path).
- Bundled zlib 1.2.13 in `libzip.so` (was 1.2.8).

## 0.5.1 — hardware + packaging

- Validated installing and running on real AmigaOne X5000 hardware.
- Installer/packaging fixes; the GitHub Releases page is the download source for
  prebuilt `.lha` archives.

## 0.5.0 — first release

- First packaged release: an Installation-Utility `.lha` assembling the VM
  (`jamvm-openjdk` + `libjvm.so`), the OpenJDK 8 class library, the OpenJDK + AWT
  native libraries, the `sun.awt.amiga` toolkit, the clib4 runtime, fonts and
  resources, a `java` launcher, and runnable examples. Runs headless Java 8
  programs and Swing applications in Workbench windows.
