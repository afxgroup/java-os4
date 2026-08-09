#!/bin/sh
# Turn an AmigaOS crash dump into function names and source lines.
#
#     tools/decode-crash.sh build/crash.txt
#     tools/decode-crash.sh < build/crash.txt
#
# The AmigaOS crash handler resolves file:line for the EXECUTABLE (clib4's crt
# is linked into it) but prints only raw addresses for shared objects:
#
#     (0x428F8470) module Work:Java/libjvm.so at 0x7F653E0C (section 0 @ 0x12B80)
#
# The information to resolve those is there -- our .so files are built with
# -gstabs and keep their .stab sections -- it just needs the section offset
# turned back into a file address.  Doing that by hand once is instructive;
# doing it for thirty frames, twice, is not.
#
# THE MAPPING
#
# "section 0 @ OFF" is an offset from the start of the loader's first section,
# which begins at the first symbol in .text -- __shlib_call_constructors, at
# .text + 4 in every shared object here.  So:
#
#     file address = OFF + (.text vaddr + 4)
#
# Confirmed against two independent dumps and both libjvm.so and libjava.so: the
# frames resolved to functions whose call chain matched the reported one exactly
# (Jam_SetIntField called from ClassLoader$NativeLibrary.load, under a
# JVM_DoPrivileged, under initClass).  If a decode ever comes out as nonsense --
# unrelated functions that could not call each other -- suspect this rule first.
#
# GPLv2 (java-os4 project).
set -e
. "$(dirname "$0")/build-env.sh" 2>/dev/null || BUILD_ROOT=$(dirname "$0")/../build

DUMP=${1:-}
if [ -n "$DUMP" ]; then
    exec < "$DUMP"
fi

ADDR2LINE=ppc-amigaos-addr2line
READELF=ppc-amigaos-readelf

# Where to look for a module named in the dump.  The dump gives AmigaOS paths
# ("Work:Java/libjvm.so"); only the basename is used, matched against what this
# tree actually built -- decoding against a DIFFERENT build than the one that
# crashed silently produces plausible, wrong answers.
search_for() {
    base=$1
    for d in "$BUILD_ROOT" \
             "$BUILD_ROOT/openjdk-natives" \
             "$BUILD_ROOT/release/Java-OS4/content/Java"; do
        [ -f "$d/$base" ] && { echo "$d/$base"; return 0; }
    done
    return 1
}

# .text vaddr + 4 -- see the note above.
section0_base() {
    # The address is the field AFTER "PROGBITS", found by name rather than by
    # position: readelf writes "[ 8]" and "[11]", so a fixed column number is
    # right for one library and off by one for the next.
    $READELF -S "$1" 2>/dev/null | awk '
        /[ \t]\.text[ \t]+PROGBITS/ {
            for (i = 1; i <= NF; i++)
                if ($i == "PROGBITS") { print "0x" $(i+1); exit }
        }'
}

echo "=== decoded frames ==="
warned_missing=""

sed -n 's/.*module [^ ]*\/\([^ ]*\) at 0x\([0-9A-Fa-f]*\) (section [0-9]* @ 0x\([0-9A-Fa-f]*\)).*/\1 \2 \3/p' \
| while read -r mod raw off; do
    path=$(search_for "$mod" || true)
    if [ -z "$path" ]; then
        case "$warned_missing" in
            *"$mod"*) ;;
            *) echo "  (no local build of $mod -- not decoded)"
               warned_missing="$warned_missing $mod" ;;
        esac
        continue
    fi

    base=$(section0_base "$path")
    if [ -z "$base" ]; then
        echo "  $mod +0x$off  (no .text section found)"
        continue
    fi

    addr=$(printf "%x" $(( base + 4 + 0x$off )))
    info=$($ADDR2LINE -f -e "$path" "0x$addr" 2>/dev/null | tr '\n' ' ')
    case "$info" in
        "?? ??:0 "|"") echo "  $mod +0x$off  (no debug info -- built without -gstabs?)" ;;
        *)             echo "  $mod +0x$off  ->  $info" ;;
    esac
done

echo
echo "The crashed instruction is the frame listed FIRST; everything below it is"
echo "its caller.  A chain whose functions could not plausibly call each other"
echo "means the address mapping is wrong, not that the stack is corrupt."
