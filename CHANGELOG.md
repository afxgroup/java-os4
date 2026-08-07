# Changelog

All notable changes to Java-OS4. Versions are AmigaOS 4 `.lha` releases on the
[Releases page](https://github.com/derfsss/java-os4/releases). Java-OS4 is a Java 8
runtime for AmigaOS 4 (PowerPC): JamVM 2.0 + the OpenJDK 8 class library + a native
`sun.awt.amiga` AWT/Swing toolkit.

## 0.6.0 — 2026-08-07

The release where a real application runs. Networking, TLS, colour management
and the AmigaOS path model all arrived or were rebuilt; the interpreter stopped
being the slowest one JamVM offers.

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
- Examples `NetTest` (walks the java.net layers bottom-up) and `NetDownload`.
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
- `java.lang.ProcessBuilder` cannot work: clib4 has neither `fork` nor `vfork`.
- `libfontmanager` leaves freetype's optional PNG, bzip2 and WOFF2 paths
  unresolved; they are unreachable with the fonts shipped.

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
