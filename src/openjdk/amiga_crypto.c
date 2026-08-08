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
#include <unistd.h>

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

/* ---- SHA-256 / SHA-512 compression ---------------------------------- *
 *
 * sun.security.provider.SHA2.implCompress0 and SHA5.implCompress0, in C.
 *
 * Unlike GHASH these buy nothing on a GCM download -- GCM authenticates with
 * GHASH and uses no HMAC, so SHA runs only during the handshake.  Where they
 * do earn their keep is the CBC suites, which HMAC every record, the key
 * schedule at connection setup, and every MessageDigest in the runtime (jar
 * verification among them).
 *
 * Cheap to do because sun.security.provider lives in rt.jar: boot-loaded, the
 * same loader as niopatch.zip, so none of the runtime-package trouble that
 * stopped AESCrypt applies here.
 *
 * The round constants are the fractional parts of the cube roots of the first
 * 80 primes, per FIPS 180-4, generated rather than transcribed -- a typo in one
 * of 144 constants would produce a digest that is wrong only sometimes.
 */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};
static const uint32_t K256[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

JNIEXPORT void JNICALL
Java_sun_security_provider_SHA5_implCompressNative(JNIEnv *env, jclass clazz,
                                                   jbyteArray bufArr, jint ofs,
                                                   jlongArray stateArr) {
    jbyte *buf;
    jlong *st;
    uint64_t W[80], a, b, c, d, e, f, g, h;
    int i;

    (void)clazz;

    if (bufArr == NULL || stateArr == NULL || ofs < 0) {
        return;
    }
    if ((*env)->GetArrayLength(env, stateArr) != 8
            || (*env)->GetArrayLength(env, bufArr) - ofs < 128) {
        return;
    }
    if ((st = (*env)->GetLongArrayElements(env, stateArr, NULL)) == NULL) {
        return;
    }
    if ((buf = (*env)->GetPrimitiveArrayCritical(env, bufArr, NULL)) == NULL) {
        (*env)->ReleaseLongArrayElements(env, stateArr, st, JNI_ABORT);
        return;
    }

    for (i = 0; i < 16; i++) {
        W[i] = getLongBE((const unsigned char *)buf + ofs + i * 8);
    }
    (*env)->ReleasePrimitiveArrayCritical(env, bufArr, buf, JNI_ABORT);

    for (i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(W[i-15], 1) ^ ROR64(W[i-15], 8) ^ (W[i-15] >> 7);
        uint64_t s1 = ROR64(W[i-2], 19) ^ ROR64(W[i-2], 61) ^ (W[i-2] >> 6);

        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }

    a = (uint64_t)st[0]; b = (uint64_t)st[1]; c = (uint64_t)st[2]; d = (uint64_t)st[3];
    e = (uint64_t)st[4]; f = (uint64_t)st[5]; g = (uint64_t)st[6]; h = (uint64_t)st[7];

    for (i = 0; i < 80; i++) {
        uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t T1 = h + S1 + ch + K512[i] + W[i];
        uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t T2 = S0 + maj;

        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    st[0] = (jlong)((uint64_t)st[0] + a); st[1] = (jlong)((uint64_t)st[1] + b);
    st[2] = (jlong)((uint64_t)st[2] + c); st[3] = (jlong)((uint64_t)st[3] + d);
    st[4] = (jlong)((uint64_t)st[4] + e); st[5] = (jlong)((uint64_t)st[5] + f);
    st[6] = (jlong)((uint64_t)st[6] + g); st[7] = (jlong)((uint64_t)st[7] + h);

    (*env)->ReleaseLongArrayElements(env, stateArr, st, 0);
}

JNIEXPORT void JNICALL
Java_sun_security_provider_SHA2_implCompressNative(JNIEnv *env, jclass clazz,
                                                   jbyteArray bufArr, jint ofs,
                                                   jintArray stateArr) {
    jbyte *buf;
    jint *st;
    uint32_t W[64], a, b, c, d, e, f, g, h;
    int i;

    (void)clazz;

    if (bufArr == NULL || stateArr == NULL || ofs < 0) {
        return;
    }
    if ((*env)->GetArrayLength(env, stateArr) != 8
            || (*env)->GetArrayLength(env, bufArr) - ofs < 64) {
        return;
    }
    if ((st = (*env)->GetIntArrayElements(env, stateArr, NULL)) == NULL) {
        return;
    }
    if ((buf = (*env)->GetPrimitiveArrayCritical(env, bufArr, NULL)) == NULL) {
        (*env)->ReleaseIntArrayElements(env, stateArr, st, JNI_ABORT);
        return;
    }

    for (i = 0; i < 16; i++) {
        const unsigned char *p = (const unsigned char *)buf + ofs + i * 4;

        W[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
             | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
    }
    (*env)->ReleasePrimitiveArrayCritical(env, bufArr, buf, JNI_ABORT);

    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(W[i-15], 7) ^ ROR32(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = ROR32(W[i-2], 17) ^ ROR32(W[i-2], 19) ^ (W[i-2] >> 10);

        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }

    a = (uint32_t)st[0]; b = (uint32_t)st[1]; c = (uint32_t)st[2]; d = (uint32_t)st[3];
    e = (uint32_t)st[4]; f = (uint32_t)st[5]; g = (uint32_t)st[6]; h = (uint32_t)st[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t T1 = h + S1 + ch + K256[i] + W[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t T2 = S0 + maj;

        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    st[0] = (jint)((uint32_t)st[0] + a); st[1] = (jint)((uint32_t)st[1] + b);
    st[2] = (jint)((uint32_t)st[2] + c); st[3] = (jint)((uint32_t)st[3] + d);
    st[4] = (jint)((uint32_t)st[4] + e); st[5] = (jint)((uint32_t)st[5] + f);
    st[6] = (jint)((uint32_t)st[6] + g); st[7] = (jint)((uint32_t)st[7] + h);

    (*env)->ReleaseIntArrayElements(env, stateArr, st, 0);
}

/* ---- entropy --------------------------------------------------------- *
 *
 * sun.security.provider.SeedGenerator's source of last resort.
 *
 * The runtime ships securerandom.source=file:/RANDOM:, which is neither of the
 * two names SeedGenerator special-cases, so it goes to URLSeedGenerator -- and
 * RANDOM: is not a device AmigaOS has.  That throws, leaving
 * ThreadedSeedGenerator, which gathers entropy by racing threads and counting
 * iterations.  On this hardware that is 25-30 seconds before the first byte of
 * any TLS connection, and it is paid again in every JVM.
 *
 * clib4 has getentropy(), so none of that racing is necessary.
 */
JNIEXPORT jboolean JNICALL
Java_sun_security_provider_SeedGenerator_nativeSeed(JNIEnv *env, jclass clazz,
                                                    jbyteArray result, jint len) {
    unsigned char chunk[256];
    jint done = 0;

    (void)clazz;

    if (result == NULL || len <= 0
            || (*env)->GetArrayLength(env, result) < len) {
        return JNI_FALSE;
    }

    /* getentropy refuses more than 256 bytes at a time, by its contract. */
    while (done < len) {
        size_t want = (size_t)(len - done);

        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        if (getentropy(chunk, want) != 0) {
            return JNI_FALSE;
        }
        (*env)->SetByteArrayRegion(env, result, done, (jsize)want, (jbyte *)chunk);
        done += (jint)want;
    }

    /* Not a security measure -- the bytes have already been copied out -- but
       leaving a buffer of live entropy on the stack costs nothing to avoid. */
    memset(chunk, 0, sizeof(chunk));
    return JNI_TRUE;
}
