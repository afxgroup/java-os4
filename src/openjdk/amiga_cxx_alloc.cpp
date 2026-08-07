/*
 * amiga_cxx_alloc.cpp -- the two C++ operators libsunec needs.
 *
 * ECC_JNI.cpp is the only C++ translation unit in the whole runtime, and it
 * uses exactly two things from the C++ runtime: operator new[] (_Znaj) and
 * operator delete[] (_ZdaPv), for plain byte buffers.  Upstream's makefile just
 * appends -lstdc++.
 *
 * We do not, for two reasons.  Linking libstdc++ dynamically would add a sobj
 * to Sobjs/ that exists to serve two symbols, and -static-libstdc++ is worse
 * still: it doubled libsunec.so to 1MB and dragged in __cxa_pure_virtual and
 * the __gthread_* family, i.e. it swapped two missing symbols for four.
 *
 * new[] here does NOT throw std::bad_alloc on exhaustion -- there is no
 * <new> and no exception machinery behind it -- it returns NULL, which is the
 * -fno-exceptions behaviour.  ECC_JNI.cpp checks its allocations against NULL
 * before use (see the ecc_jni.cpp allocation sites, which all guard), so this
 * is the contract it already expects.
 */

#include <stdlib.h>

void *operator new[](unsigned int size) {
    /* Zero-sized allocations must still return a distinct, freeable pointer. */
    if (size == 0) {
        size = 1;
    }
    return malloc(size);
}

void operator delete[](void *ptr) {
    /* delete[] on NULL is defined to be a no-op, and so is free(NULL). */
    free(ptr);
}
