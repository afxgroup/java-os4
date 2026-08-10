/*
 * FileBusy -- why can't this file be deleted or replaced?
 *
 *     java -cp examples/FileBusy.jar FileBusy Work:Invoicex/plugins/Some.jar
 *     java -cp examples/FileBusy.jar FileBusy            (self-test, no target)
 *
 * On AmigaOS a file that is OPEN cannot be deleted, renamed over, or reopened
 * for writing: DOS answers ERROR_OBJECT_IN_USE, which arrives in Java as
 *
 *     java.io.FileNotFoundException: <path> (Device busy)
 *
 * POSIX allows all three, so code that replaces a file in place works on Linux
 * and fails here -- and it fails in a way that reads like a path problem and is
 * not one.  An updater hits this replacing its own jars.
 *
 * The question that decides what to do about it is WHO holds the file, and it
 * has exactly two answers with opposite fixes:
 *
 *   - another process   -> nothing this JVM does will help; the holder must
 *                          close it or exit
 *   - THIS JVM          -> something opened it and did not close it, and on
 *                          this runtime that matters more than on Linux: an
 *                          unclosed ZipFile stays open until its finalizer
 *                          runs, and the default heap is now large enough that
 *                          a short-lived program may never collect at all
 *
 * The self-test settles the second case by construction: it opens a jar, drops
 * the reference WITHOUT closing it, and tries to delete a copy of it -- then
 * runs a collection and tries again.  If the second attempt succeeds where the
 * first failed, unclosed ZipFiles are the mechanism, and the fix is to close
 * them (or force a collection) before replacing anything.
 *
 * GPLv2 (java-os4 project).
 */
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.zip.ZipFile;

public class FileBusy {

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            report(new File(args[0]));
        } else {
            selfTest();
        }
    }

    /* ---- report on a real file ------------------------------------------ */

    private static void report(File f) {
        System.out.println("file   : " + f.getPath());
        System.out.println("exists : " + f.exists());
        if (!f.exists()) {
            System.out.println();
            System.out.println("Nothing holds a file that is not there -- if a"
                               + " replace failed, the destination is elsewhere.");
            return;
        }
        System.out.println("length : " + f.length());
        System.out.println("canRead: " + f.canRead());
        System.out.println("canWrite: " + f.canWrite());
        System.out.println();

        /* Opening for APPEND asks DOS for the same exclusive access a replace
           needs, without truncating the file if it turns out to be free. */
        System.out.println("open for write : " + tryOpenForWrite(f));
        System.out.println("delete()       : " + f.delete()
                           + (f.exists() ? "   (still there)" : "   (gone)"));

        if (!f.exists()) {
            System.out.println();
            System.out.println("It deleted, so nothing was holding it.");
            return;
        }

        System.out.println();
        System.out.println("Held.  Trying a collection, in case it is THIS JVM"
                           + " holding it through an unclosed ZipFile:");
        System.gc();
        System.runFinalization();
        try {
            Thread.sleep(500);
        } catch (InterruptedException ignored) {
            /* nothing */
        }

        System.out.println("  open for write : " + tryOpenForWrite(f));
        System.out.println("  delete()       : " + f.delete());
        System.out.println();
        if (!f.exists()) {
            System.out.println("A collection freed it -- so this JVM had it open"
                + " and had dropped the reference. Close the ZipFile/JarFile"
                + " explicitly before replacing the file.");
        } else {
            System.out.println("Still held after a collection, so it is NOT an"
                + " unclosed handle in this JVM. Another process has it open --"
                + " most likely one that has not exited, or one that leaked the"
                + " handle. Nothing this program does can release it.");
        }
    }

    private static String tryOpenForWrite(File f) {
        RandomAccessFile raf = null;
        try {
            raf = new RandomAccessFile(f, "rw");
            return "ok (not held)";
        } catch (Exception e) {
            return e.toString();
        } finally {
            if (raf != null) {
                try {
                    raf.close();
                } catch (IOException ignored) {
                    /* nothing */
                }
            }
        }
    }

    /* ---- self-test: does an unclosed ZipFile block deletion? ------------- */

    private static void selfTest() throws Exception {
        System.out.println("FileBusy self-test");
        System.out.println("does an unclosed ZipFile stop its file being"
                           + " deleted on this runtime?");
        System.out.println();

        File tmp = File.createTempFile("filebusy", ".zip");
        writeMinimalZip(tmp);

        /*
         * Opened and then dropped, deliberately, with no close().  That is the
         * shape the problem takes in real code -- nobody writes it on purpose,
         * it happens when an exception skips the close or a helper returns the
         * entries and forgets the file.
         */
        openAndAbandon(tmp);

        System.out.println("with a ZipFile open and unreferenced:");
        System.out.println("  delete() -> " + tmp.delete()
                           + (tmp.exists() ? "   (still there)" : "   (gone)"));

        if (!tmp.exists()) {
            System.out.println();
            System.out.println("Deleted anyway -- an open ZipFile does NOT block"
                + " deletion here, so an unclosed handle is not the mechanism"
                + " and the holder is some other process.");
            return;
        }

        System.out.println();
        System.out.println("after System.gc() + runFinalization():");
        System.gc();
        System.runFinalization();
        Thread.sleep(500);
        System.out.println("  delete() -> " + tmp.delete()
                           + (tmp.exists() ? "   (still there)" : "   (gone)"));

        System.out.println();
        if (!tmp.exists()) {
            System.out.println("CONFIRMED: an unclosed ZipFile blocks deletion,"
                + " and finalizing it releases the file. Any code replacing a"
                + " jar must close it first -- and on this runtime a large heap"
                + " means finalizers may not run on their own for a long time.");
        } else {
            System.out.println("Still there even after finalizing, which points"
                + " at something else holding it -- worth re-running with the"
                + " path as an argument to see the errors themselves.");
            tmp.deleteOnExit();
        }
    }

    private static void openAndAbandon(File f) throws IOException {
        ZipFile z = new ZipFile(f);
        z.entries();                  /* touch it, so nothing optimises it away */
        /* no close, and no reference kept */
    }

    /* The smallest thing ZipFile will accept: an empty archive, which is just
       an end-of-central-directory record. */
    private static void writeMinimalZip(File f) throws IOException {
        FileOutputStream out = new FileOutputStream(f);
        try {
            out.write(new byte[] {
                0x50, 0x4b, 0x05, 0x06,
                0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0
            });
        } finally {
            out.close();
        }
    }
}
