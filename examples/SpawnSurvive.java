/*
 * SpawnSurvive -- does a spawned JVM outlive the one that spawned it?
 *
 *     java -cp examples/SpawnSurvive.jar SpawnSurvive
 *
 * Reproduces, in about twenty seconds, what InvoiceX's auto-update takes a full
 * install-and-update cycle to show: a Java process starts a second Java process
 * and then exits, and the second one dies the instant the first does.
 *
 * The serial log of the real failure says where:
 *
 *     [EXIT] amiga_exit: returning to main (status=0)     <- parent gone
 *     DSI exception ... findArchiveDirEntry (zip.c:285)   <- child, next instant
 *
 * so the child was reading a jar's central directory when its memory went away.
 * That is why the child here does NOT do its work up front: it waits for the
 * parent to be gone and only THEN loads classes it has not touched yet, which
 * is the operation that actually fails.  A child that finishes before its
 * parent proves nothing.
 *
 * The child writes to a FILE, not to stdout.  A detached process has NIL: for
 * its stdio by design -- there is nobody left to read a pipe -- so the console
 * would show nothing either way, and a test whose result you cannot see is
 * worse than no test.
 *
 *     java -cp examples/SpawnSurvive.jar SpawnSurvive dirty
 *
 * runs the same thing with the parent leaving a stuck non-daemon thread, so its
 * shutdown is forced out on a timeout the way a real application's is rather
 * than draining cleanly the way a test's does.
 *
 *     PASS   the file ends with SURVIVED
 *     FAIL   the file stops partway  -> the child died when the parent did
 *     FAIL   the file is never made  -> the child never started
 *
 * GPLv2 (java-os4 project).
 */
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class SpawnSurvive {

    /* Long enough that the parent is unquestionably gone -- its shutdown alone
       takes several seconds on this runtime, most of it draining threads. */
    private static final int WAIT_FOR_PARENT_TO_DIE_MS = 8000;

    private static final String MARKER = "SURVIVED";

    public static void main(String[] args) throws Exception {
        if (args.length >= 2 && "child".equals(args[0])) {
            child(args[1]);
            return;
        }
        parent(args.length > 0 && "dirty".equals(args[0]));
    }

    /* ---- parent ---------------------------------------------------------- */

    /*
     * dirty: leave a non-daemon thread stuck, so the parent exits the way a real
     * application does rather than the way a test does.
     *
     * The difference is visible in the serial log and it is not small.  This
     * test's parent drains and goes:
     *
     *     [EXIT] wait loop done tries=1   threads=1 target=1
     *
     * InvoiceX's could not, and left on a timeout with two threads still alive:
     *
     *     [EXIT] wait loop done tries=301 threads=2 target=1
     *     [EXIT] amiga_exit: calling exit() (threads left=2)
     *
     * A parent that exits cleanly and one that is forced out after three
     * seconds of failing to drain are different shutdowns, and the child sees
     * different things on the way down.  If the real case ever fails again
     * while the plain test passes, run this mode before suspecting the spawn.
     */
    private static void parent(boolean dirty) throws Exception {
        File out = new File("spawnsurvive.txt");
        if (out.exists() && !out.delete()) {
            System.out.println("cannot remove the previous " + out
                               + " -- is it still locked by an earlier run?");
            System.exit(2);
        }

        String java = javaCommand();
        String cp = System.getProperty("java.class.path");

        List<String> cmd = new ArrayList<String>();
        cmd.add(java);
        cmd.add("-cp");
        cmd.add(cp);
        cmd.add("SpawnSurvive");
        cmd.add("child");
        cmd.add(out.getAbsolutePath());

        System.out.println("SpawnSurvive"
                           + (dirty ? "  [dirty shutdown]" : ""));
        System.out.println("  launcher : " + java);
        System.out.println("  result   : " + out.getAbsolutePath());
        System.out.println();
        System.out.println("starting the child, then exiting immediately.");

        if (dirty) {
            /* Non-daemon and parked forever: the VM cannot drain it, so the
               shutdown takes the timeout path.  Not a daemon thread -- a daemon
               would be abandoned quietly and prove nothing. */
            Thread stuck = new Thread(new Runnable() {
                public void run() {
                    synchronized (SpawnSurvive.class) {
                        try {
                            SpawnSurvive.class.wait();
                        } catch (InterruptedException ignored) {
                            /* nothing: the point is not to leave */
                        }
                    }
                }
            }, "stuck-nondaemon");
            stuck.setDaemon(false);
            stuck.start();
            System.out.println("  mode     : dirty (a stuck non-daemon thread,"
                               + " so shutdown takes the timeout path)");
        }

        new ProcessBuilder(cmd).start();

        /*
         * No waitFor, deliberately: waiting is the one thing that would hide
         * the bug, because the child would finish while the parent still
         * existed.  The parent's whole job here is to go away.
         */
        System.out.println("parent exiting now -- the child has "
                           + (WAIT_FOR_PARENT_TO_DIE_MS / 1000)
                           + "s of waiting still to do.");
        System.out.println();
        System.out.println("check " + out.getName() + " in about "
                           + ((WAIT_FOR_PARENT_TO_DIE_MS / 1000) + 15)
                           + " seconds:");
        System.out.println("  last line " + MARKER + "  -> the child survived");
        System.out.println("  file stops earlier       -> it died with the parent");
        System.out.println("  no file at all           -> it never started");

        /* Explicit, because in dirty mode the stuck thread would otherwise keep
           the VM alive: the point is for this process to GO, by whichever path
           the mode asked for. */
        System.exit(0);
    }

    /*
     * $JAVA_HOME/bin/java -- the path an application builds for itself, which
     * is the one worth testing.  Falls back to plain "java" so this still runs
     * where java.home is not set the way a JRE lays out.
     */
    private static String javaCommand() {
        String home = System.getProperty("java.home");
        if (home != null && home.length() > 0) {
            File candidate = new File(new File(home, "bin"), "java");
            if (candidate.exists()) {
                return candidate.getPath();
            }
            candidate = new File(home, "java");
            if (candidate.exists()) {
                return candidate.getPath();
            }
        }
        return "java";
    }

    /* ---- child ----------------------------------------------------------- */

    private static void child(String path) {
        PrintWriter log = null;
        try {
            /* Unbuffered and flushed after every line: if the process is killed
               mid-way, what it managed to do must already be on disk, or the
               file cannot tell "died here" from "never got there". */
            log = new PrintWriter(new FileWriter(path), true);

            log.println("child started, pid unknown to Java");
            log.println("waiting " + WAIT_FOR_PARENT_TO_DIE_MS
                        + "ms for the parent to exit");

            long start = System.currentTimeMillis();
            while (System.currentTimeMillis() - start < WAIT_FOR_PARENT_TO_DIE_MS) {
                Thread.sleep(500);
                log.println("  still here at "
                            + (System.currentTimeMillis() - start) + "ms");
            }

            log.println("parent should be gone now -- starting the real work");

            /*
             * Classes deliberately not touched before this point, so every one
             * of them is a fresh read of a jar's central directory AFTER the
             * parent has exited.  That is the operation the crash log names
             * (findArchiveDirEntry), and doing it earlier would let it succeed
             * while the parent was still alive.
             */
            String[] classes = {
                "java.util.regex.Pattern",
                "java.text.SimpleDateFormat",
                "java.util.zip.ZipFile",
                "javax.xml.parsers.DocumentBuilderFactory",
                "java.security.MessageDigest",
                "java.util.concurrent.ConcurrentHashMap",
                "java.net.URLConnection",
                "javax.swing.JPanel",
                "java.awt.geom.AffineTransform",
                "java.util.Formatter",
            };

            for (int i = 0; i < classes.length; i++) {
                Class.forName(classes[i]);
                log.println("  loaded " + classes[i]);
            }

            /* And some allocation, since the other failures were a hash table
               growing rather than a class loading. */
            log.println("allocating");
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 200000; i++) {
                sb.append(i % 10);
                if (sb.length() > 100000) {
                    sb.setLength(0);
                }
            }
            log.println("  allocated ok");

            log.println(MARKER);
        } catch (Throwable t) {
            if (log != null) {
                log.println("FAILED: " + t);
                t.printStackTrace(log);
            }
        } finally {
            if (log != null) {
                log.close();
            }
        }
    }
}
