/*
 * Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/*
 * (C) Copyright IBM Corp. 2013
 */

package com.sun.crypto.provider;

import java.security.*;
import javax.crypto.*;
import static com.sun.crypto.provider.AESConstants.AES_BLOCK_SIZE;

/**
 * This class represents the GCTR function defined in NIST 800-38D
 * under section 6.5. It needs to be constructed w/ an initialized
 * cipher object, and initial counter block(ICB). Given an input X
 * of arbitrary length, it processes and returns an output which has
 * the same length as X. The invariants of this class are:
 *
 * (1) The length of intialCounterBlk (and also of its clones, e.g.,
 * fields counter and counterSave) is equal to AES_BLOCK_SIZE.
 *
 * (2) After construction, the field counter never becomes null, it
 * always contains a byte array of length AES_BLOCK_SIZE.
 *
 * If any invariant is broken, failures can occur because the
 * AESCrypt.encryptBlock method can be intrinsified on the HotSpot VM
 * (see JDK-8067648 for details).
 *
 * <p>This function is used in the implementation of GCM mode.
 *
 * @since 1.8
 */
final class GCTR {

    // these fields should not change after the object has been constructed
    private final SymmetricCipher aes;
    private final byte[] icb;

    // the current counter value
    private byte[] counter;

    // needed for save/restore calls
    private byte[] counterSave = null;

    // NOTE: cipher should already be initialized
    GCTR(SymmetricCipher cipher, byte[] initialCounterBlk) {
        this.aes = cipher;
        if (initialCounterBlk.length != AES_BLOCK_SIZE) {
            throw new RuntimeException("length of initial counter block (" + initialCounterBlk.length +
                                       ") not equal to AES_BLOCK_SIZE (" + AES_BLOCK_SIZE + ")");
        }
        this.icb = initialCounterBlk;
        this.counter = icb.clone();
    }

    // input must be multiples of 128-bit blocks when calling update
    int update(byte[] in, int inOfs, int inLen, byte[] out, int outOfs) {
        if (inLen - inOfs > in.length) {
            throw new RuntimeException("input length out of bound");
        }
        if (inLen < 0 || inLen % AES_BLOCK_SIZE != 0) {
            throw new RuntimeException("input length unsupported");
        }
        if (out.length - outOfs < inLen) {
            throw new RuntimeException("output buffer too small");
        }

        /*
         * AmigaOS: the whole record in one native call.
         *
         * Not at AESCrypt.encryptBlock, where HotSpot puts its intrinsic: an
         * intrinsic is inlined machine code and can afford to be per-block,
         * while a JNI call cannot -- 16 bytes per crossing is some 65000
         * transitions per megabyte.  Here the block loop stays in C and the
         * boundary is crossed once per record.
         *
         * Returns -1 for anything it does not handle, and then the stock loop
         * below runs: a key length it does not know, or an array it cannot
         * pin, has to degrade rather than corrupt a stream.
         */
        if (nativeUsable && aes instanceof AESCrypt) {
            byte[] rawKey = ((AESCrypt) aes).amigaRawKey();

            if (rawKey != null) {
                try {
                    if (updateNative(rawKey, counter, in, inOfs, inLen,
                                     out, outOfs) == inLen) {
                        return inLen;
                    }
                } catch (Throwable t) {
                    /*
                     * A runtime without the native throws UnsatisfiedLinkError
                     * here, not -1 -- and this jar has to keep working on one.
                     * Remembered rather than retried: the answer cannot change,
                     * and catching it per record would cost more than the
                     * native saves.
                     */
                    nativeUsable = false;
                }
            }
        }

        byte[] encryptedCntr = new byte[AES_BLOCK_SIZE];

        int numOfCompleteBlocks = inLen / AES_BLOCK_SIZE;
        for (int i = 0; i < numOfCompleteBlocks; i++) {
            aes.encryptBlock(counter, 0, encryptedCntr, 0);
            for (int n = 0; n < AES_BLOCK_SIZE; n++) {
                int index = (i * AES_BLOCK_SIZE + n);
                out[outOfs + index] =
                    (byte) ((in[inOfs + index] ^ encryptedCntr[n]));
            }
            GaloisCounterMode.increment32(counter);
        }
        return inLen;
    }

    /**
     * AmigaOS: counter mode over a whole buffer, in src/openjdk/amiga_crypto.c.
     * Advances counter in place with GCM's 32-bit increment.  Returns inLen, or
     * -1 if it declined -- the caller then does the work itself.
     */
    private static native int updateNative(byte[] key, byte[] counter,
                                           byte[] in, int inOfs, int inLen,
                                           byte[] out, int outOfs);

    /** Cleared for good the first time the native turns out not to be there. */
    private static volatile boolean nativeUsable = true;

    // input can be arbitrary size when calling doFinal
    protected int doFinal(byte[] in, int inOfs, int inLen, byte[] out,
                          int outOfs) throws IllegalBlockSizeException {
        try {
            if (inLen < 0) {
                throw new IllegalBlockSizeException("Negative input size!");
            } else if (inLen > 0) {
                int lastBlockSize = inLen % AES_BLOCK_SIZE;
                int completeBlkLen = inLen - lastBlockSize;
                // process the complete blocks first
                update(in, inOfs, completeBlkLen, out, outOfs);
                if (lastBlockSize != 0) {
                    // do the last partial block
                    byte[] encryptedCntr = new byte[AES_BLOCK_SIZE];
                    aes.encryptBlock(counter, 0, encryptedCntr, 0);
                    for (int n = 0; n < lastBlockSize; n++) {
                        out[outOfs + completeBlkLen + n] =
                            (byte) ((in[inOfs + completeBlkLen + n] ^
                                     encryptedCntr[n]));
                    }
                }
            }
        } finally {
            reset();
        }
        return inLen;
    }

    /**
     * Resets the content of this object to when it's first constructed.
     */
    void reset() {
        System.arraycopy(icb, 0, counter, 0, icb.length);
        counterSave = null;
    }

    /**
     * Save the current content of this object.
     */
    void save() {
        this.counterSave = this.counter.clone();
    }

    /**
     * Restores the content of this object to the previous saved one.
     */
    void restore() {
        if (this.counterSave != null) {
            this.counter = this.counterSave;
        }
    }
}
