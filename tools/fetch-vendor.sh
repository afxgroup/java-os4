#!/bin/sh
# Fetch the vendored upstream source trees Java-OS4 builds from.
#
# These trees are large and gitignored -- only OUR changes are tracked (as the
# patch in docs/) -- so a fresh clone needs this one-time step.  Everything here
# is PUBLIC:
#   * vendor/jamvm     <- github.com/jaokim/jamiga-jamvm
#                         + docs/jamvm-amiga-openjdk.patch  (our 11 commits)
#   * vendor/icedtea8  <- github.com/jaokim/jamiga-icedtea8-3.0
#
# Run from the repo root:   sh tools/fetch-vendor.sh      (or:  make vendor)
# Needs: git + network (runs on the host, not in the build container).
set -e
cd "$(dirname "$0")/.."                 # repo root
PATCH_FILE="$(pwd)/docs/jamvm-amiga-openjdk.patch"

apply_jamvm_patch_if_needed() {
    if git -C vendor/jamvm apply --check "$PATCH_FILE" >/dev/null 2>&1; then
        echo "=== applying docs/jamvm-amiga-openjdk.patch (our AmigaOS4 + OpenJDK 8 changes) ==="
        git -C vendor/jamvm apply "$PATCH_FILE"
        echo "    patch applied to vendor/jamvm working tree"
    elif git -C vendor/jamvm apply --reverse --check "$PATCH_FILE" >/dev/null 2>&1; then
        echo "=== docs/jamvm-amiga-openjdk.patch already applied -- skipping ==="
    else
        # Neither direction applies.  The usual cause is NOT a corrupt checkout:
        # it is a tree still carrying an OLDER revision of this same patch.  The
        # forward check then fails (already patched) and the reverse check fails
        # too (patched to a different revision), so the old "continue as-is"
        # branch silently froze vendor/jamvm at whatever patch level it had --
        # which is how the classpath-separator fix (the amiga_classpath.h include
        # in src/os/amiga/os.c) went missing while docs/ already carried it.
        #
        # That case is provable and repairable: if some historical revision of
        # this patch reverse-applies exactly, the tree is that revision, and we
        # can rewind it and re-apply the current one with nothing to lose.
        for old in $(git log --format=%h -- "$PATCH_FILE" 2>/dev/null); do
            git show "$old:docs/jamvm-amiga-openjdk.patch" > "vendor/jamvm/.old-patch" 2>/dev/null || continue
            if git -C vendor/jamvm apply --reverse --check .old-patch >/dev/null 2>&1; then
                echo "=== vendor/jamvm is at an OLDER revision of the patch ($old) -- resyncing ==="
                git -C vendor/jamvm apply --reverse .old-patch
                rm -f vendor/jamvm/.old-patch
                git -C vendor/jamvm apply "$PATCH_FILE"
                echo "    rewound to $old and re-applied the current patch"
                # src/classlib*.h are copies the VM build makes of the chosen
                # classlib's headers, and "-I ." makes them win over the originals.
                # Stale copies would shadow the freshly patched ones.
                rm -f vendor/jamvm/src/classlib.h vendor/jamvm/src/classlib-defs.h \
                      vendor/jamvm/src/classlib-excep.h vendor/jamvm/src/classlib-symbol.h
                return 0
            fi
        done
        rm -f vendor/jamvm/.old-patch
        echo "WARNING: docs/jamvm-amiga-openjdk.patch does not apply cleanly to this jamvm checkout,"
        echo "         and it matches no previous revision of the patch either."
        echo "         Diverging files:"
        git -C vendor/jamvm apply --numstat "$PATCH_FILE" 2>/dev/null | cut -f3 | while read -r f; do
            [ -n "$f" ] || continue
            awk -v F="$f" 'BEGIN{p=0} /^diff --git /{p=($0 ~ "a/"F"$" || $0 ~ "a/"F" ")} p' \
                "$PATCH_FILE" > vendor/jamvm/.hunk.patch
            git -C vendor/jamvm apply --reverse --check .hunk.patch >/dev/null 2>&1 || echo "           $f"
        done
        rm -f vendor/jamvm/.hunk.patch
        echo "         Continuing with vendor/jamvm as-is."
        echo "         If the VM build fails later, reset vendor/jamvm to the expected upstream revision and rerun make vendor."
    fi
}

# --- JamVM (the engine): jaokim's JAmiga fork + our AmigaOS4 / OpenJDK 8 work --
if [ -d vendor/jamvm/.git ]; then
    echo "=== vendor/jamvm already present ==="
    apply_jamvm_patch_if_needed
elif [ -d vendor/jamvm ]; then
    echo "ERROR: vendor/jamvm exists but is not a git repository."
    echo "       Remove vendor/jamvm and rerun make vendor."
    exit 1
else
    echo "=== cloning jamiga-jamvm -> vendor/jamvm ==="
    git clone https://github.com/jaokim/jamiga-jamvm vendor/jamvm
    apply_jamvm_patch_if_needed
fi

# --- IcedTea 8 harness: produces the OpenJDK 8 source the natives compile from --
if [ -d vendor/icedtea8/.git ]; then
    echo "=== vendor/icedtea8 already present -- skipping ==="
else
    echo "=== cloning jamiga-icedtea8-3.0 -> vendor/icedtea8 ==="
    git clone https://github.com/jaokim/jamiga-icedtea8-3.0 vendor/icedtea8
fi

cat <<'EOF'

=== vendor sources ready ===
  vendor/jamvm     -- the JamVM engine (with our AmigaOS 4 + OpenJDK 8 patch)
  vendor/icedtea8  -- the IcedTea 8 build harness

ONE heavyweight step remains before `make natives`: build the OpenJDK 8 source
tree the native libraries compile from.  Run the IcedTea harness (see
vendor/icedtea8/README + HACKING) to download + extract OpenJDK 8u into
build/openjdk8/.  The native build scripts expect that source at
build/openjdk8/jdk-<changeset>; set J= in tools/build-openjdk-natives.sh and
tools/build-awt-natives.sh to match the directory it produces.

The VM (`make vm`) and clib4 (`make clib4`) do NOT need that step -- only the
OpenJDK native libraries do.  See docs/BUILDING.md for the full flow.
EOF
