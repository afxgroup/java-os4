/*
 * amiga_aes.c -- AES counter mode, in a library the extension loader owns.
 *
 * Separate from amiga_crypto.c for one reason, and it is not tidiness.  A
 * native method binds only to libraries loaded by ITS OWN class loader:
 *
 *     static long findNative(ClassLoader loader, String name) {
 *         Vector<NativeLibrary> libs =
 *             loader != null ? loader.nativeLibraries : systemNativeLibraries;
 *
 * There is no fallback to the system libraries.  GHASH is on the boot class
 * path (niopatch.zip), so loader == null and it finds libjava.so.  GCTR comes
 * from lib/ext/sunjce_provider.jar via the extension loader, whose library list
 * is empty -- so its native could never resolve out of libjava.so no matter how
 * correct the code was.
 *
 * That is exactly what happened: the counters showed GCTR calls=0 while GHASH
 * ran 540804 blocks, and the UnsatisfiedLinkError was being swallowed by a
 * catch(Throwable) meant for robustness.  The measurement found it; the
 * "native" label from reflection had said everything was fine for three builds.
 *
 * So this goes in its own .so, which GCTR loads itself -- registering it under
 * the extension loader, where its native can be found.
 *
 * GPLv2 (java-os4 project).
 */

#include <stdint.h>
#include <string.h>

#include "jni.h"

static volatile unsigned long gctr_calls = 0;      /* entered */
static volatile unsigned long gctr_declined = 0;   /* returned -1 */
static volatile unsigned long gctr_bytes = 0;      /* actually processed */

/* Read back by CryptoBench through GCTR, not AmigaDiag: AmigaDiag is
   boot-loaded and lives in libjava.so, which has its own copy of nothing. */
JNIEXPORT jlongArray JNICALL
Java_com_sun_crypto_provider_GCTR_nativeStats(JNIEnv *env, jclass clazz) {
    jlong v[3];
    jlongArray out;

    (void)clazz;
    v[0] = (jlong)gctr_calls;
    v[1] = (jlong)gctr_declined;
    v[2] = (jlong)gctr_bytes;

    if ((out = (*env)->NewLongArray(env, 3)) != NULL) {
        (*env)->SetLongArrayRegion(env, out, 0, 3, v);
    }
    return out;
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
    gctr_calls++;

    if (keyArr == NULL || counterArr == NULL || inArr == NULL || outArr == NULL) {
        gctr_declined++;
        return -1;
    }
    if (inLen <= 0 || inLen % 16 != 0 || inOfs < 0 || outOfs < 0) {
        gctr_declined++;
        return -1;
    }

    keyLen = (*env)->GetArrayLength(env, keyArr);
    if (keyLen != 16 && keyLen != 24 && keyLen != 32) {
        gctr_declined++;
        return -1;
    }
    if ((*env)->GetArrayLength(env, counterArr) != 16
            || (*env)->GetArrayLength(env, inArr) - inOfs < inLen
            || (*env)->GetArrayLength(env, outArr) - outOfs < inLen) {
        gctr_declined++;
        return -1;
    }

    if (!te_ready) {
        buildTables();
    }

    (*env)->GetByteArrayRegion(env, keyArr, 0, keyLen, (jbyte *)key);
    (*env)->GetByteArrayRegion(env, counterArr, 0, 16, (jbyte *)counter);

    if ((rounds = expandKey(key, keyLen, rk)) == 0) {
        gctr_declined++;
        return -1;
    }

    /* Critical on both: one TLS record is up to 16KB, and copying it twice
       would hand back a good part of what the C loop just won. */
    if ((inp = (*env)->GetPrimitiveArrayCritical(env, inArr, NULL)) == NULL) {
        gctr_declined++;
        return -1;
    }
    if ((outp = (*env)->GetPrimitiveArrayCritical(env, outArr, NULL)) == NULL) {
        (*env)->ReleasePrimitiveArrayCritical(env, inArr, inp, JNI_ABORT);
        gctr_declined++;
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
    gctr_bytes += (unsigned long)inLen;
    return inLen;
}
