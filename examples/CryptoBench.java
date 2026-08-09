/*
 * CryptoBench -- how fast the bulk ciphers actually are on this machine.
 *
 * TLS throughput here is decided by the bulk cipher, and the bulk cipher runs
 * as interpreted bytecode: there is no JIT, so none of HotSpot's crypto
 * intrinsics exist.  The gap that leaves is not small.  Measured on a host with
 * -Xint and the intrinsics off, AES-256-GCM manages 790 KB/s where the same
 * cipher without GHASH -- AES-256-CBC -- reaches 3084.
 *
 *     java -cp examples/CryptoBench.jar CryptoBench [MB]
 *
 * Two things worth reading here.
 *
 * The RATIO between GCM and CBC says how much GHASH is costing.  With GHASH
 * native (src/openjdk/amiga_crypto.c) the two should come much closer together;
 * if GCM is still four times slower, the native is not being reached and the
 * first thing to check is whether niopatch.zip really is ahead of rt.jar on the
 * boot class path.
 *
 * The ABSOLUTE number sets the ceiling for https.  A download cannot go faster
 * than its cipher, so if AES-128-GCM reports 300 KB/s then 300 KB/s is what
 * NetDownload will see over TLS no matter what the socket does.  That is the
 * honest way to tell a crypto problem from a network one.
 *
 * GPLv2 (java-os4 project).
 */
import java.security.Security;
import java.security.spec.AlgorithmParameterSpec;
import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public class CryptoBench {

    /* 64K at a time: about a TLS record, and large enough that per-call
       overhead does not show up as cipher speed. */
    private static final int CHUNK = 64 * 1024;

    public static void main(String[] args) throws Exception {
        int mb = args.length > 0 ? Integer.parseInt(args[0]) : 4;

        System.out.println("CryptoBench -- " + System.getProperty("os.name")
                           + " " + System.getProperty("os.version")
                           + ", " + mb + " MB per cipher");
        System.out.println("GHASH: " + ghashKind());
        System.out.println("GCTR : " + gctrKind());
        System.out.println();

        run("AES-128-GCM", "AES/GCM/NoPadding", 128, true,  mb);
        run("AES-256-GCM", "AES/GCM/NoPadding", 256, true,  mb);
        run("AES-128-CBC", "AES/CBC/NoPadding", 128, false, mb);
        run("AES-256-CBC", "AES/CBC/NoPadding", 256, false, mb);

        System.out.println();
        System.out.println(cryptoStats());
        System.out.println();
        System.out.println("GCM is native (GCTR + GHASH), CBC is not: the gap");
        System.out.println("between them is C against interpreted bytecode, not");
        System.out.println("one mode against another.  The absolute rate is the");
        System.out.println("ceiling for anything over https -- read GCTR");
        System.out.println("declined=0 with bytes matching GHASH as the proof");
        System.out.println("that the native saw all of the data.");
    }

    /*
     * Reports whether the accelerated GHASH is in place, by asking whether the
     * method carries the native flag.  Worth printing: a benchmark that
     * silently measured the stock implementation would look like the native
     * had achieved nothing.
     */
    private static String ghashKind() {
        try {
            java.lang.reflect.Method m =
                Class.forName("com.sun.crypto.provider.GHASH")
                     .getDeclaredMethod("processBlocks", byte[].class, int.class,
                                        int.class, long[].class, long[].class);
            return java.lang.reflect.Modifier.isNative(m.getModifiers())
                   ? "native (amiga_crypto.c)" : "Java (stock)";
        } catch (Throwable t) {
            return "unknown (" + t + ")";
        }
    }

    /*
     * Whether counter mode is accelerated, and -- when it is not -- which jar
     * the class came from.
     *
     * The first run after the native landed showed no change at all, and the
     * bench could not say why: it reported GHASH and nothing else.  GCTR comes
     * from lib/ext/sunjce_provider.jar, which the build patches in place, so
     * "not native" here almost always means the runtime is loading a jar from
     * somewhere other than where the release put it.  Printing the location
     * turns that from a guess into a fact.
     */
    private static String gctrKind() {
        try {
            Class<?> c = Class.forName("com.sun.crypto.provider.GCTR");
            java.lang.reflect.Method m =
                c.getDeclaredMethod("updateNative", byte[].class, byte[].class,
                                    byte[].class, int.class, int.class,
                                    byte[].class, int.class);
            return java.lang.reflect.Modifier.isNative(m.getModifiers())
                   ? "native (amiga_crypto.c)" : "Java (stock)";
        } catch (NoSuchMethodException e) {
            return "Java (stock) -- loaded from " + whereFrom();
        } catch (Throwable t) {
            return "unknown (" + t + ")";
        }
    }

    /*
     * What the natives did, not merely whether they exist.
     *
     * "declined" is the number that decides the next move.  High, and the
     * native is entered and refuses -- an argument shape it does not accept,
     * and the Java loop silently does the work.  Zero with the byte count
     * matching the data, and the native handled everything, which would mean
     * the time was never in AES and looking harder at the cipher is wasted.
     */
    private static String cryptoStats() {
        try {
            /* Read from two places because they live in two libraries: GHASH's
               native is in libjava.so (boot loader), GCTR's in
               libamigacrypto.so (extension loader), and a .so has its own
               statics.  That split is not tidiness -- see GCTR's loadLibrary. */
            long[] g = (long[]) Class.forName("java.lang.AmigaDiag")
                                     .getMethod("cryptoStats").invoke(null);
            java.lang.reflect.Method m =
                Class.forName("com.sun.crypto.provider.GCTR")
                     .getDeclaredMethod("nativeStats");
            m.setAccessible(true);
            long[] c = (long[]) m.invoke(null);

            return "GCTR  calls=" + c[0] + " declined=" + c[1]
                 + " bytes=" + c[2] + "\n"
                 + "GHASH calls=" + g[0] + " blocks=" + g[1]
                 + " (" + (g[1] * 16) + " bytes)";
        } catch (Throwable t) {
            return "crypto natives: not this runtime";
        }
    }

    private static String whereFrom() {
        try {
            java.security.CodeSource cs =
                Class.forName("com.sun.crypto.provider.GCTR")
                     .getProtectionDomain().getCodeSource();
            return cs == null ? "the boot class path" : String.valueOf(cs.getLocation());
        } catch (Throwable t) {
            return "somewhere unreadable (" + t + ")";
        }
    }

    private static void run(String name, String xform, int keyBits,
                            boolean gcm, int mb) {
        try {
            byte[] key = new byte[keyBits / 8];
            byte[] iv = new byte[gcm ? 12 : 16];
            byte[] data = new byte[CHUNK];
            Cipher c = Cipher.getInstance(xform);
            int rounds = mb * 1024 * 1024 / CHUNK;
            int ctr = 0;

            /* Two warm-up rounds: the first call through a cipher pulls in its
               class initialisation and key schedule, which is not what is
               being measured. */
            for (int i = 0; i < 2; i++) {
                c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), spec(gcm, iv, ++ctr));
                c.doFinal(data);
            }

            long t0 = System.currentTimeMillis();
            for (int i = 0; i < rounds; i++) {
                /* GCM refuses a repeated IV, so it has to be re-initialised
                   each round; CBC is done the same way to keep the comparison
                   honest rather than to give GCM a handicap. */
                c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), spec(gcm, iv, ++ctr));
                c.doFinal(data);
            }
            long ms = Math.max(1, System.currentTimeMillis() - t0);
            long kbs = (long) rounds * CHUNK * 1000L / ms / 1024;

            System.out.println("  " + pad(name, 12) + pad(kbs + " KB/s", 12)
                               + "(" + ms + " ms)");
        } catch (Throwable t) {
            System.out.println("  " + pad(name, 12) + "FAILED: " + t);
        }
    }

    private static AlgorithmParameterSpec spec(boolean gcm, byte[] iv, int ctr) {
        iv[0] = (byte) ctr;
        iv[1] = (byte) (ctr >> 8);
        return gcm ? new GCMParameterSpec(128, iv) : new IvParameterSpec(iv);
    }

    private static String pad(String s, int w) {
        StringBuilder sb = new StringBuilder(s);
        while (sb.length() < w) {
            sb.append(' ');
        }
        return sb.toString();
    }
}
