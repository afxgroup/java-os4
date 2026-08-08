/*
 * amiga_crypto.c -- the crypto inner loops, in C instead of bytecode.
 *
 * TLS bulk throughput on this port is decided by AES-GCM, and AES-GCM is
 * decided by GHASH.  Measured interpreted, AES-256-GCM runs at 790 KB/s where
 * AES-256-CBC -- the same cipher without GHASH -- manages 3084.  The whole
 * difference is one function.
 *
 * On a JIT this never shows, because HotSpot replaces exactly these methods
 * with intrinsics.  There is no JIT here, so the same job is done by making the
 * methods native.  The set is the one HotSpot targets, which is not a
 * coincidence: it is where the time is.
 *
 *     com.sun.crypto.provider.GHASH.processBlocks     <- this file
 *     com.sun.crypto.provider.AESCrypt.encryptBlock
 *     sun.security.provider.SHA2/SHA5.implCompress
 *
 * Paired with src/niopatch/com/sun/crypto/provider/GHASH.java, which declares
 * the method native.  No new JCE provider is involved -- a third-party one
 * would have to be signed by Oracle on JDK 8 -- we are replacing the guts of
 * SunJCE, which is already registered and trusted.
 *
 * GPLv2 (java-os4 project).
 */

#include <stdint.h>
#include <string.h>

#include "jni.h"

/*
 * GF(2^128) multiply, GCM's reduction polynomial, bit-reflected as the spec
 * has it: the "leftmost" bit of the block is bit 0 of the field element, which
 * is why the reduction constant is 0xe1000... at the TOP of the low word and
 * the shift goes right.
 *
 * This is a faithful translation of GHASH.blockMult -- same loop, same masks,
 * no algorithmic liberties.  The gain is not a better algorithm, it is not
 * being interpreted: 128 iterations of shift/mask/xor per 16 bytes cost a few
 * hundred bytecodes each, and there are 65536 of them in a 1MB transfer.
 *
 * A 4-bit table would beat this again, but it has to be rebuilt whenever the
 * subkey changes and processBlocks is handed the subkey rather than owning it.
 * Worth doing only if the measurement says this is still the bottleneck.
 */
static void blockMult(uint64_t st[2], const uint64_t subH[2]) {
    uint64_t Z0 = 0, Z1 = 0;
    uint64_t V0 = subH[0], V1 = subH[1];
    uint64_t X;
    int i;

    X = st[0];
    for (i = 0; i < 64; i++) {
        /* Arithmetic shift of a signed value: all-ones when the top bit is
           set, zero otherwise.  The Java does the same with >> on a long. */
        uint64_t mask = (uint64_t)((int64_t)X >> 63);
        uint64_t carry;

        Z0 ^= V0 & mask;
        Z1 ^= V1 & mask;

        mask = (uint64_t)(((int64_t)(V1 << 63)) >> 63);

        carry = V0 & 1;
        V0 >>= 1;
        V1 = (V1 >> 1) | (carry << 63);

        V0 ^= 0xe100000000000000ULL & mask;
        X <<= 1;
    }

    X = st[1];
    for (i = 64; i < 127; i++) {
        uint64_t mask = (uint64_t)((int64_t)X >> 63);
        uint64_t carry;

        Z0 ^= V0 & mask;
        Z1 ^= V1 & mask;

        mask = (uint64_t)(((int64_t)(V1 << 63)) >> 63);

        carry = V0 & 1;
        V0 >>= 1;
        V1 = (V1 >> 1) | (carry << 63);

        V0 ^= 0xe100000000000000ULL & mask;
        X <<= 1;
    }

    /* Z128: the last bit contributes but V is not stepped again. */
    {
        uint64_t mask = (uint64_t)((int64_t)X >> 63);

        Z0 ^= V0 & mask;
        Z1 ^= V1 & mask;
    }

    st[0] = Z0;
    st[1] = Z1;
}

/* Big-endian, matching GHASH.getLong -- NOT the host's byte order. */
static uint64_t getLongBE(const unsigned char *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48)
         | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
         | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16)
         | ((uint64_t)p[6] <<  8) | ((uint64_t)p[7]);
}

/*
 * Signature fixed by the Java side: it is the shape HotSpot's intrinsic uses,
 * and the class is patched, not rewritten.
 *
 * The bounds were already checked by the caller (GHASH.processBlocksCheck),
 * which the stock comment is explicit about -- this method "cannot throw
 * exceptions or allocate arrays as it will breaking intrinsics".  We hold to
 * the same contract, and re-check the two array lengths anyway because getting
 * that wrong here is a wild write rather than an exception.
 */
JNIEXPORT void JNICALL
Java_com_sun_crypto_provider_GHASH_processBlocks(JNIEnv *env, jclass clazz,
                                                 jbyteArray data, jint inOfs,
                                                 jint blocks, jlongArray stArr,
                                                 jlongArray subHArr) {
    jbyte *bytes;
    jlong *st_j, *subH_j;
    uint64_t st[2], subH[2];
    jint i;

    (void)clazz;

    if (data == NULL || stArr == NULL || subHArr == NULL || blocks <= 0) {
        return;
    }
    if ((*env)->GetArrayLength(env, stArr) != 2
            || (*env)->GetArrayLength(env, subHArr) != 2) {
        return;
    }
    if (inOfs < 0 || blocks > (((*env)->GetArrayLength(env, data) - inOfs) / 16)) {
        return;
    }

    if ((st_j = (*env)->GetLongArrayElements(env, stArr, NULL)) == NULL) {
        return;
    }
    if ((subH_j = (*env)->GetLongArrayElements(env, subHArr, NULL)) == NULL) {
        (*env)->ReleaseLongArrayElements(env, stArr, st_j, JNI_ABORT);
        return;
    }
    /* Critical, not the copying accessor: this is the hot path and the array
       is only read.  A copy per call would give back part of what the native
       just won. */
    if ((bytes = (*env)->GetPrimitiveArrayCritical(env, data, NULL)) == NULL) {
        (*env)->ReleaseLongArrayElements(env, subHArr, subH_j, JNI_ABORT);
        (*env)->ReleaseLongArrayElements(env, stArr, st_j, JNI_ABORT);
        return;
    }

    st[0]   = (uint64_t)st_j[0];
    st[1]   = (uint64_t)st_j[1];
    subH[0] = (uint64_t)subH_j[0];
    subH[1] = (uint64_t)subH_j[1];

    for (i = 0; i < blocks; i++) {
        const unsigned char *p = (const unsigned char *)bytes + inOfs + i * 16;

        st[0] ^= getLongBE(p);
        st[1] ^= getLongBE(p + 8);
        blockMult(st, subH);
    }

    (*env)->ReleasePrimitiveArrayCritical(env, data, bytes, JNI_ABORT);

    st_j[0] = (jlong)st[0];
    st_j[1] = (jlong)st[1];

    (*env)->ReleaseLongArrayElements(env, subHArr, subH_j, JNI_ABORT);
    (*env)->ReleaseLongArrayElements(env, stArr, st_j, 0);
}
