#!/bin/sh
# Build the sun.awt.amiga AWT toolkit (Phase 4 M4) -> amigatoolkit.zip,
# and the SwingTest app -> swingtest.zip.
# Compiles against the Temurin 8 rt.jar (the runtime class library).
set -e
. "$(dirname "$0")/build-env.sh"
JDK8=$BOOT_JDK
RT="$JDK8/jre/lib/rt.jar"
SRC=$PROJECT_ROOT/src/amigaawt/java
OUT=$BUILD_ROOT/amigatoolkit
mkdir -p "$OUT/classes" "$OUT/testclasses"

"$JDK8/bin/javac" -source 8 -target 8 -encoding UTF-8 \
    -bootclasspath "$RT" -XDignore.symbol.file=true \
    -d "$OUT/classes" \
    "$SRC"/sun/awt/amiga/*.java

(cd "$OUT/classes" && "$JDK8/bin/jar" cf "$BUILD_ROOT/amigatoolkit.zip" sun)
echo "amigatoolkit.zip OK ($(wc -c < "$BUILD_ROOT/amigatoolkit.zip") bytes)"

"$JDK8/bin/javac" -source 8 -target 8 -encoding UTF-8 \
    -bootclasspath "$RT" \
    -d "$OUT/testclasses" \
    "$PROJECT_ROOT/tests/gui/SwingTest.java" "$PROJECT_ROOT/tests/gui/SwingType.java" "$PROJECT_ROOT/tests/gui/SwingResize.java" "$PROJECT_ROOT/tests/gui/SwingApp.java" "$PROJECT_ROOT/tests/gui/SwingDialog.java" "$PROJECT_ROOT/tests/gui/SwingModal.java"

(cd "$OUT/testclasses" && "$JDK8/bin/jar" cf "$BUILD_ROOT/swingtest.zip" .)
echo "swingtest.zip OK ($(wc -c < "$BUILD_ROOT/swingtest.zip") bytes)"

# injin: guest-side input injector (real-input GUI testing)
ppc-amigaos-gcc -mcrt=clib4 -O2 -Wall -o "$BUILD_ROOT/injin" \
    "$PROJECT_ROOT/src/tools/injin.c"
echo "injin OK ($(wc -c < "$BUILD_ROOT/injin") bytes)"
