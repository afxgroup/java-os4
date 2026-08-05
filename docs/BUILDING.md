# Building Java-OS4

Everything cross-compiles inside a Docker image that carries the AmigaOS 4
PowerPC toolchain and a host JDK 8. You do not need an Amiga to build — only to
run the result.

## Prerequisites

- **Docker.** The build image (`tools/Dockerfile`) derives from
  [`walkero/amigagccondocker:os4-gcc11`](https://hub.docker.com/r/walkero/amigagccondocker)
  — the public AmigaOS 4 cross-toolchain image (`ppc-amigaos-gcc` 11.5.0 + SDK +
  clib4) — and adds a host JDK 8 + `ecj`. The base is pulled automatically the
  first time you build the image.
- **The clib4 C runtime** — vendored as the **`clib4/` git submodule**
  (`AmigaLabs/clib4`, `development` branch). Check it out with
  `git submodule update --init` (or clone the repo with `--recursive`). The build
  compiles clib4 from this submodule, so the runtime always matches the build.
- **The OpenJDK 8 native-source tree** — tracked as the **`vendor/openjdk8`** git
  submodule, pinned to the project source level (`jdk8u77-b03`, matching the
  JDK `3334efeacd83` tree used by the native build scripts). Check it out with
  `git submodule update --init vendor/openjdk8`.
- **The vendored JamVM + IcedTea 8 harness** — public upstream trees, gitignored
  here (only our changes are tracked, as the patch under `docs/`). Fetch them
  once with **`make vendor`** (or `sh tools/fetch-vendor.sh`): it clones
  [`jaokim/jamiga-jamvm`](https://github.com/jaokim/jamiga-jamvm) into
  `vendor/jamvm` and applies `docs/jamvm-amiga-openjdk.patch`, and clones
  [`jaokim/jamiga-icedtea8-3.0`](https://github.com/jaokim/jamiga-icedtea8-3.0)
  into `vendor/icedtea8`.

Build the image once (rebuild when `tools/Dockerfile` changes):

```sh
docker build -t javaos4-build:latest -f tools/Dockerfile .
```

> **Windows:** run Docker from PowerShell (call `docker.exe` directly). The
> Git-Bash/MSYS layer rewrites the `-v`/`-w` paths and breaks the mounts.

## Local host build (no Docker)

If you already have the AmigaOS 4 cross toolchain installed locally, you can run
the same pipeline directly on the host with `Makefile.local` instead of Docker.
This path reuses the installed SDK clib4 and does not fetch or rebuild the
in-repo `clib4/` tree.

Prerequisites:

- `ppc-amigaos-gcc` and the AmigaOS SDK on `PATH`
- `clib4` already installed in the SDK, typically `/usr/ppc-amigaos/SDK/clib4`
- a host **JDK 8** with `javah` available; point `BOOT_JDK` at its home if it is
  not on `JAVA_HOME`
- the OpenJDK 8 native sources, preferably via the tracked `vendor/openjdk8`
  git submodule pinned to the project source level (`jdk8u77-b03`, matching the
  JDK `3334efeacd83` tree used here); alternatively pass
  `OPENJDK8_SRC=/path/to/jdk-*`

Example:

```sh
git submodule update --init vendor/openjdk8
make vendor
make -f Makefile.local build \
  SDK_BASE=/usr/ppc-amigaos/SDK \
  BOOT_JDK=/opt/jdk8 \
  OPENJDK8_SRC=$PWD/vendor/openjdk8

make -f Makefile.local dist \
  SDK_BASE=/usr/ppc-amigaos/SDK \
  BOOT_JDK=/opt/jdk8 \
  OPENJDK8_SRC=$PWD/vendor/openjdk8
```

`make -f Makefile.local check-local` validates those paths before running the
build scripts.

### OpenJDK patch reproducibility

To make submodule builds reproducible across clones, the OpenJDK native-source
adaptations are tracked in `docs/openjdk8-amiga.patch` and auto-applied by the
native build scripts when possible (`git apply` check). If the patch is already
applied, scripts continue without changes; if it cannot be applied cleanly, the
existing idempotent script-side adaptations are still executed.

### Strict unresolved-symbol diagnostics

`tools/build-openjdk-natives.sh` supports an optional strict linker mode:

```sh
STRICT_NO_UNDEFINED=nio make -f Makefile.local natives
```

Values:
- `off` (default): normal build behavior.
- `nio`: enable `-Wl,--no-undefined` only for `libnio.so`.
- `all`: enable it for all OpenJDK native `.so` built by the script.

Note: `all` is intentionally noisy in this project model because several
symbols are expected to resolve at runtime across VM/runtime libraries.

In the commands below the working directory is the repository root.

## Build steps

The `Makefile` drives the cross build (each target runs the matching `tools/`
script inside the image and writes to `build/`):

```sh
git submodule update --init clib4 vendor/openjdk8
make vendor                       # fetch JamVM + IcedTea 8 upstream sources, once
make image                        # build the build image (pulls walkero base), once
make build                        # clib4 + VM + OpenJDK/AWT natives + toolkit
make dist                         # gather build/ -> build/release/Java/ + .lha
make release                      # build then dist, in one step
```

`make build` expands to, in order: `build-jamvm-openjdk.sh` (the VM:
`libjvm.so` + the `jamvm-openjdk` launcher), `build-openjdk-natives.sh`
(`libjava`/`libzip`/`libnio`/`libnet`/…), `build-awt-natives.sh` +
`build-amigaawt.sh` (`libawt`/`libfontmanager` + the Amiga windowing JNI), and
`build-amigatoolkit.sh` (the `sun.awt.amiga` toolkit + test zips). `make dist`
runs `package.sh`, which only gathers existing `build/` outputs — it compiles
nothing and needs no clib4 mount. You can still invoke any `tools/` script by
hand inside the image if you prefer.

The class-library jars (`rt.jar`, `charsets.jar`, …) come from the Temurin 8 JDK
in the build image; `package.sh` gathers them along with the VM, the native
libraries, the clib4/support shared objects, the font data, and the launcher.

### Getting the OpenJDK 8 native sources

`make natives` compiles `libjava` / `libawt` / `libfontmanager` / … from the
OpenJDK 8 C sources from `vendor/openjdk8` by default. That submodule is the
preferred source for both Docker and local builds. If you still want to use the
older IcedTea-generated extracted tree under `build/openjdk8/jdk-<changeset>`,
set `OPENJDK8_SRC` explicitly before invoking the build scripts or
`Makefile.local`. (`make vm` and `make clib4` do **not** need this step — only
the OpenJDK native libraries do.)

## The VM as a shared library

On AmigaOS 4 an executable does not export symbols to `dlopen`'d objects, so the
VM is built as **`libjvm.so`** (exporting `JVM_*` / `JNI_*`) and the `java`
launcher links against it. The OpenJDK native libraries resolve their VM symbols
against `libjvm.so` at load time, and all components share one clib4 instance via
clib4's shared-library model (`-use-dynld` launcher + plain `-shared` natives,
with the clib4 `.so` objects shipped alongside).

## Running

A built release is a self-contained `Java/` drawer (see the top-level
[README](../README.md) for end-user install/run). A few things matter when
deploying or testing:

- **Run via a real CLI process.** clib4's `pthread_create` needs a proper
  process context (`CreateNewProc`), so launch through `Run`/`Execute`, e.g.
  `Run Execute <script>`, rather than a non-interactive remote exec.
- **The boot classpath is relative.** JamVM's boot-classpath parser splits on
  `:`, which collides with AmigaOS `Volume:` names, so the default boot classpath
  is colon-free and resolved against the current directory. The `java` launcher
  therefore `CD`s into the `Java:` drawer before starting the VM.
- **Shared objects.** The launcher sets `LD_LIBRARY_PATH` so the bundled clib4
  and support `.so` files load from the install drawer. Ship these with the app;
  do **not** overwrite the system `SOBJS:`.
- **Execute bit.** After copying a binary onto the target, ensure its `e`
  (execute) protection bit is set (`Protect <file> +e`) — without it AmigaDOS
  silently refuses to run it.

### Testing on QEMU

The fast development loop uses an AmigaOS 4 PowerPC QEMU machine
(`qemu-system-ppc -M amigaone`, with `graphics.library`-capable display).
Transfer the install drawer to the guest, assign `Java:` to it, and run the
launcher. The `tests/` directory holds self-verifying programs (conformance
suites and GUI tests) that print `[PASS]`/`[FAIL]` lines you can capture from a
redirected output file.

### Testing on hardware

The primary bring-up target is an AmigaOne X5000. Copy the `Java/` drawer over,
assign `Java:`, and run as above.

## Versioning

The project version is in the top-level `VERSION` file and stamped into the
release archive name. Releases are tagged `vMAJOR.MINOR.PATCH`.
