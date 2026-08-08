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
 * two names SeedGenerator special-cases, so it goes to URLSeedGenerator.
 *
 * RANDOM: is a perfectly real AmigaOS device and open/fopen read it without
 * trouble -- what fails is Java's file: URL layer, which reports
 *
 *     Failed to create seed generator with file:/RANDOM::
 *     java.io.IOException: Failed to open file:/RANDOM:
 *
 * and leaves ThreadedSeedGenerator, which gathers entropy by racing threads
 * and counting iterations.  Making the URL path work would be the other repair;
 * this one avoids needing it.  On this hardware that is 25-30 seconds before the first byte of
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

/* ---- AES counter mode ------------------------------------------------ *
 *
 * com.sun.crypto.provider.GCTR.update, in C.
 *
 * With GHASH native, AES is what the remaining TLS throughput is made of: on
 * the Amiga all four bulk ciphers land in one narrow band (436-631 KB/s),
 * which is the signature of a shared cost rather than of any one mode.
 *
 * Accelerated at update() -- a whole TLS record -- and NOT at
 * AESCrypt.encryptBlock, which is where HotSpot puts its intrinsic.  HotSpot
 * can afford per-block because an intrinsic is inlined machine code; a JNI
 * call is not, and 16 bytes per crossing would be some 65000 transitions per
 * megabyte.  One crossing per record instead, with the block loop in C.
 *
 * libcrypto was the intended engine and is not usable: it is 3.4MB and needs
 * libatomic.so, which the SDK does not have, so shipping it would fail at
 * load.  This is a T-table AES instead -- the same construction OpenSSL's
 * portable path uses -- with no new dependency.  Encryption only: counter mode
 * is symmetric, so GCM never calls AES decryption even when decrypting.
 *
 * The counter is ours to advance, deliberately.  GCM increments the low 32
 * bits (GaloisCounterMode.increment32) while AES-CTR as libcrypto implements
 * it carries through all 128; they agree until the low word wraps, which for a
 * 16KB record is a 1-in-4-million event per record and a near-certainty across
 * a large enough download.  Cheap to get right, expensive to debug.
 */

static const unsigned char AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* Built once from the S-box rather than written out: 4KB of literals is 4KB of
   opportunity to mistype one, and the construction is three lines. */
static uint32_t Te0[256], Te1[256], Te2[256], Te3[256];
static int te_ready = 0;

static unsigned char xtime(unsigned char x) {
    return (unsigned char)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void buildTables(void) {
    int i;

    for (i = 0; i < 256; i++) {
        unsigned char s = AES_SBOX[i];
        unsigned char s2 = xtime(s);
        unsigned char s3 = (unsigned char)(s2 ^ s);

        Te0[i] = ((uint32_t)s2 << 24) | ((uint32_t)s << 16)
               | ((uint32_t)s << 8)  | (uint32_t)s3;
        Te1[i] = (Te0[i] >> 8) | (Te0[i] << 24);
        Te2[i] = (Te1[i] >> 8) | (Te1[i] << 24);
        Te3[i] = (Te2[i] >> 8) | (Te2[i] << 24);
    }
    te_ready = 1;
}

#define GET32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) \
                | ((uint32_t)(p)[2] << 8)  | (uint32_t)(p)[3])
#define PUT32(p, v) do { (p)[0] = (unsigned char)((v) >> 24); \
                         (p)[1] = (unsigned char)((v) >> 16); \
                         (p)[2] = (unsigned char)((v) >> 8);  \
                         (p)[3] = (unsigned char)(v); } while (0)

/* Returns the number of rounds, or 0 for a key length AES does not define. */
static int expandKey(const unsigned char *key, int keyLen, uint32_t rk[60]) {
    static const uint32_t RCON[10] = {
        0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000,
        0x20000000, 0x40000000, 0x80000000, 0x1b000000, 0x36000000
    };
    int nk = keyLen / 4;
    int rounds = nk + 6;
    int total = 4 * (rounds + 1);
    int i;

    if (nk != 4 && nk != 6 && nk != 8) {
        return 0;
    }
    for (i = 0; i < nk; i++) {
        rk[i] = GET32(key + 4 * i);
    }
    for (i = nk; i < total; i++) {
        uint32_t t = rk[i - 1];

        if (i % nk == 0) {
            t = (t << 8) | (t >> 24);
            t = ((uint32_t)AES_SBOX[(t >> 24) & 0xff] << 24)
              | ((uint32_t)AES_SBOX[(t >> 16) & 0xff] << 16)
              | ((uint32_t)AES_SBOX[(t >> 8)  & 0xff] << 8)
              |  (uint32_t)AES_SBOX[t & 0xff];
            t ^= RCON[i / nk - 1];
        } else if (nk == 8 && i % nk == 4) {
            t = ((uint32_t)AES_SBOX[(t >> 24) & 0xff] << 24)
              | ((uint32_t)AES_SBOX[(t >> 16) & 0xff] << 16)
              | ((uint32_t)AES_SBOX[(t >> 8)  & 0xff] << 8)
              |  (uint32_t)AES_SBOX[t & 0xff];
        }
        rk[i] = rk[i - nk] ^ t;
    }
    return rounds;
}

static void encryptBlock(const uint32_t *rk, int rounds,
                         const unsigned char in[16], unsigned char out[16]) {
    uint32_t s0, s1, s2, s3, t0, t1, t2, t3;
    int r;

    s0 = GET32(in)      ^ rk[0];
    s1 = GET32(in + 4)  ^ rk[1];
    s2 = GET32(in + 8)  ^ rk[2];
    s3 = GET32(in + 12) ^ rk[3];

    for (r = 1; r < rounds; r++) {
        const uint32_t *k = rk + 4 * r;

        t0 = Te0[(s0>>24)&0xff] ^ Te1[(s1>>16)&0xff] ^ Te2[(s2>>8)&0xff] ^ Te3[s3&0xff] ^ k[0];
        t1 = Te0[(s1>>24)&0xff] ^ Te1[(s2>>16)&0xff] ^ Te2[(s3>>8)&0xff] ^ Te3[s0&0xff] ^ k[1];
        t2 = Te0[(s2>>24)&0xff] ^ Te1[(s3>>16)&0xff] ^ Te2[(s0>>8)&0xff] ^ Te3[s1&0xff] ^ k[2];
        t3 = Te0[(s3>>24)&0xff] ^ Te1[(s0>>16)&0xff] ^ Te2[(s1>>8)&0xff] ^ Te3[s2&0xff] ^ k[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }

    /* Final round: SubBytes/ShiftRows with no MixColumns, so the tables do not
       apply and the S-box is used directly. */
    {
        const uint32_t *k = rk + 4 * rounds;

        t0 = ((uint32_t)AES_SBOX[(s0>>24)&0xff] << 24) | ((uint32_t)AES_SBOX[(s1>>16)&0xff] << 16)
           | ((uint32_t)AES_SBOX[(s2>>8)&0xff] << 8)   |  (uint32_t)AES_SBOX[s3&0xff];
        t1 = ((uint32_t)AES_SBOX[(s1>>24)&0xff] << 24) | ((uint32_t)AES_SBOX[(s2>>16)&0xff] << 16)
           | ((uint32_t)AES_SBOX[(s3>>8)&0xff] << 8)   |  (uint32_t)AES_SBOX[s0&0xff];
        t2 = ((uint32_t)AES_SBOX[(s2>>24)&0xff] << 24) | ((uint32_t)AES_SBOX[(s3>>16)&0xff] << 16)
           | ((uint32_t)AES_SBOX[(s0>>8)&0xff] << 8)   |  (uint32_t)AES_SBOX[s1&0xff];
        t3 = ((uint32_t)AES_SBOX[(s3>>24)&0xff] << 24) | ((uint32_t)AES_SBOX[(s0>>16)&0xff] << 16)
           | ((uint32_t)AES_SBOX[(s1>>8)&0xff] << 8)   |  (uint32_t)AES_SBOX[s2&0xff];
        PUT32(out,      t0 ^ k[0]);
        PUT32(out + 4,  t1 ^ k[1]);
        PUT32(out + 8,  t2 ^ k[2]);
        PUT32(out + 12, t3 ^ k[3]);
    }
}

/*
 * Returns the number of bytes processed, or -1 to say "not handled" -- the
 * Java side then runs its own loop.  Failing soft matters: a key length we do
 * not recognise, or an array we cannot pin, must degrade to the stock
 * implementation rather than corrupt a TLS stream.
 */
JNIEXPORT jint JNICALL
Java_com_sun_crypto_provider_GCTR_updateNative(JNIEnv *env, jclass clazz,
                                               jbyteArray keyArr,
                                               jbyteArray counterArr,
                                               jbyteArray inArr, jint inOfs,
                                               jint inLen, jbyteArray outArr,
                                               jint outOfs) {
    uint32_t rk[60];
    unsigned char key[32], counter[16], keystream[16];
    jbyte *inp, *outp;
    jint keyLen, blocks, i;
    int rounds;

    (void)clazz;

    if (keyArr == NULL || counterArr == NULL || inArr == NULL || outArr == NULL) {
        return -1;
    }
    if (inLen <= 0 || inLen % 16 != 0 || inOfs < 0 || outOfs < 0) {
        return -1;
    }

    keyLen = (*env)->GetArrayLength(env, keyArr);
    if (keyLen != 16 && keyLen != 24 && keyLen != 32) {
        return -1;
    }
    if ((*env)->GetArrayLength(env, counterArr) != 16
            || (*env)->GetArrayLength(env, inArr) - inOfs < inLen
            || (*env)->GetArrayLength(env, outArr) - outOfs < inLen) {
        return -1;
    }

    if (!te_ready) {
        buildTables();
    }

    (*env)->GetByteArrayRegion(env, keyArr, 0, keyLen, (jbyte *)key);
    (*env)->GetByteArrayRegion(env, counterArr, 0, 16, (jbyte *)counter);

    if ((rounds = expandKey(key, keyLen, rk)) == 0) {
        return -1;
    }

    /* Critical on both: one TLS record is up to 16KB, and copying it twice
       would hand back a good part of what the C loop just won. */
    if ((inp = (*env)->GetPrimitiveArrayCritical(env, inArr, NULL)) == NULL) {
        return -1;
    }
    if ((outp = (*env)->GetPrimitiveArrayCritical(env, outArr, NULL)) == NULL) {
        (*env)->ReleasePrimitiveArrayCritical(env, inArr, inp, JNI_ABORT);
        return -1;
    }

    blocks = inLen / 16;
    for (i = 0; i < blocks; i++) {
        const unsigned char *src = (const unsigned char *)inp + inOfs + i * 16;
        unsigned char *dst = (unsigned char *)outp + outOfs + i * 16;
        int n;

        encryptBlock(rk, rounds, counter, keystream);
        for (n = 0; n < 16; n++) {
            dst[n] = (unsigned char)(src[n] ^ keystream[n]);
        }

        /* GaloisCounterMode.increment32: the low 32 bits only.  Carrying into
           the upper twelve bytes, as a general-purpose CTR would, diverges
           from GCM the moment this word wraps. */
        {
            int j;

            for (j = 15; j >= 12; j--) {
                if (++counter[j] != 0) {
                    break;
                }
            }
        }
    }

    (*env)->ReleasePrimitiveArrayCritical(env, outArr, outp, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, inArr, inp, JNI_ABORT);

    (*env)->SetByteArrayRegion(env, counterArr, 0, 16, (jbyte *)counter);

    memset(key, 0, sizeof(key));
    memset(rk, 0, sizeof(rk));
    memset(keystream, 0, sizeof(keystream));
    return inLen;
}
