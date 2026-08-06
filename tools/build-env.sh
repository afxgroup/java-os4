#!/bin/sh
# Shared path resolution for Docker and host builds.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PROJECT_ROOT=${PROJECT_ROOT:-$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)}
BUILD_ROOT=${BUILD_ROOT:-$PROJECT_ROOT/build}
if [ -z "${BOOT_JDK:-}" ] && [ -x "$BUILD_ROOT/boot-jdk8/bin/javah" ]; then
    BOOT_JDK=$BUILD_ROOT/boot-jdk8
fi
BOOT_JDK=${BOOT_JDK:-${JAVA_HOME:-/opt/jdk8}}

if [ -z "${SDK_BASE:-}" ]; then
    for candidate in /usr/ppc-amigaos/SDK /opt/ppc-amigaos/ppc-amigaos/SDK; do
        if [ -d "$candidate" ]; then
            SDK_BASE=$candidate
            break
        fi
    done
fi
[ -n "${SDK_BASE:-}" ] || SDK_BASE=/opt/ppc-amigaos/ppc-amigaos/SDK

SDK_CLIB4=${SDK_CLIB4:-$SDK_BASE/clib4}
PROJECT_CLIB4_ROOT=${PROJECT_CLIB4_ROOT:-$PROJECT_ROOT/clib4}
if [ -z "${CLIB4_BUILD_ROOT:-}" ] && [ -d "$PROJECT_CLIB4_ROOT/build/lib" ]; then
    CLIB4_BUILD_ROOT=$PROJECT_CLIB4_ROOT
fi
CLIB4_RUNTIME_ROOT=${CLIB4_RUNTIME_ROOT:-${CLIB4_BUILD_ROOT:-$SDK_CLIB4}}

OPENJDK8_ROOT=${OPENJDK8_ROOT:-$BUILD_ROOT/openjdk8}
if [ -z "${OPENJDK8_SRC:-}" ] && [ -d "$PROJECT_ROOT/vendor/openjdk8/jdk/src" ]; then
    OPENJDK8_SRC=$PROJECT_ROOT/vendor/openjdk8/jdk
fi
if [ -z "${OPENJDK8_SRC:-}" ] && [ -d "$OPENJDK8_ROOT" ]; then
    OPENJDK8_SRC=$(find "$OPENJDK8_ROOT" -mindepth 1 -maxdepth 1 -type d -name 'jdk-*' | sort | head -n 1)
fi

SDK_LOCAL_COMMON_INCLUDE=${SDK_LOCAL_COMMON_INCLUDE:-$SDK_BASE/local/common/include}
SDK_LOCAL_FREETYPE_INCLUDE=${SDK_LOCAL_FREETYPE_INCLUDE:-$SDK_LOCAL_COMMON_INCLUDE/freetype2}
SDK_LOCAL_CLIB4_LIB=${SDK_LOCAL_CLIB4_LIB:-$SDK_BASE/local/clib4/lib}

export PROJECT_ROOT BUILD_ROOT BOOT_JDK SDK_BASE SDK_CLIB4 PROJECT_CLIB4_ROOT
export CLIB4_BUILD_ROOT CLIB4_RUNTIME_ROOT OPENJDK8_ROOT OPENJDK8_SRC
export SDK_LOCAL_COMMON_INCLUDE SDK_LOCAL_FREETYPE_INCLUDE SDK_LOCAL_CLIB4_LIB

apply_openjdk_patch() {
    PATCH_FILE=${OPENJDK8_PATCH_FILE:-$PROJECT_ROOT/docs/openjdk8-amiga.patch}
    [ -f "$PATCH_FILE" ] || return 0

    if ! command -v git >/dev/null 2>&1; then
        echo "WARN: git not found; skipping OpenJDK patch apply"
        return 0
    fi

    OJ_SRC=${OPENJDK8_SRC:-}
    [ -n "$OJ_SRC" ] || return 0

    OJ_ROOT=$(git -C "$OJ_SRC" rev-parse --show-toplevel 2>/dev/null || true)
    if [ -z "$OJ_ROOT" ]; then
        OJ_PARENT=$(dirname "$OJ_SRC")
        OJ_ROOT=$(git -C "$OJ_PARENT" rev-parse --show-toplevel 2>/dev/null || true)
    fi
    if [ -z "$OJ_ROOT" ]; then
        echo "WARN: cannot locate OpenJDK git root for $OJ_SRC"
        return 0
    fi

    if git -C "$OJ_ROOT" apply --check "$PATCH_FILE" >/dev/null 2>&1; then
        git -C "$OJ_ROOT" apply "$PATCH_FILE"
        echo "=== applied OpenJDK patch: $PATCH_FILE ==="
    elif git -C "$OJ_ROOT" apply --reverse --check "$PATCH_FILE" >/dev/null 2>&1; then
        echo "=== OpenJDK patch already applied: $PATCH_FILE ==="
    else
        # Neither direction applies => the vendored tree is somewhere in between:
        # partially adapted, so it matches neither the pristine nor the patched
        # state.  That normally means a sed block in build-openjdk-natives.sh grew
        # a new edit while its guard ("has this file been touched at all?") was
        # already satisfied by an earlier run, so the new edit never fired.
        # Name the files, otherwise this warning says nothing actionable.
        echo "WARN: cannot apply OpenJDK patch cleanly ($PATCH_FILE); continuing with script-side adaptations"
        echo "WARN:   diverging files (adapted, but not to the state the patch describes):"
        git -C "$OJ_ROOT" apply --numstat "$PATCH_FILE" 2>/dev/null | cut -f3 | while read -r f; do
            [ -n "$f" ] || continue
            awk -v F="$f" 'BEGIN{p=0} /^diff --git /{p=($0 ~ "a/"F"$" || $0 ~ "a/"F" ")} p' \
                "$PATCH_FILE" > "$OJ_ROOT/.oj-hunk.patch"
            if ! git -C "$OJ_ROOT" apply --reverse --check .oj-hunk.patch >/dev/null 2>&1; then
                echo "WARN:     $f"
            fi
        done
        rm -f "$OJ_ROOT/.oj-hunk.patch"
        echo "WARN:   to resync: restore them (git -C $OJ_ROOT checkout -- <file>) and re-run the build,"
        echo "WARN:   or apply the missing hunks by hand, then regenerate the patch."
    fi
}