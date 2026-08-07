/*
 * ExecTest -- exercises Runtime.exec()/ProcessBuilder.
 *
 * Until this port grew a UNIXProcess implementation, the very first exec threw
 *
 *     java.lang.Error: AmigaOS is not a supported OS platform.
 *         at java.lang.UNIXProcess$Platform.get(UNIXProcess.java:164)
 *
 * from a static initialiser, before any native ran.  The Java half now knows
 * about AmigaOS (src/niopatch/java/lang/UNIXProcess.java) and the native half
 * is built on clib4's spawnvpe (src/openjdk/amiga_process.c).
 *
 *     java -cp examples/ExecTest.jar ExecTest
 *
 * The checks go bottom-up so a failure names the layer that broke rather than
 * just "exec does not work": can we launch at all, does the exit code come
 * back, do the three pipes carry bytes in each direction, does the working
 * directory and the environment reach the child, does a missing program raise
 * IOException instead of something stranger.
 *
 * Everything is driven through the shell commands AmigaOS ships with, so
 * nothing outside the base install is needed.  A check whose command is
 * missing is SKIPPED, not failed -- the point is to test exec, not to insist
 * on a particular C: drawer.
 *
 * GPLv2 (java-os4 project).
 */
import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.List;
import java.util.Map;

public class ExecTest {

    private static int passed = 0;
    private static int failed = 0;
    private static int skipped = 0;

    /* AmigaOS ships these in C:.  On a Unix host the same names mostly exist
       too, which keeps the test runnable where it is developed. */
    private static final String ECHO = "echo";
    private static final String CD   = "cd";

    public static void main(String[] args) {
        System.out.println("ExecTest -- Runtime.exec on "
                           + System.getProperty("os.name") + " "
                           + System.getProperty("os.version"));
        System.out.println();

        canLaunch();
        exitCodeZero();
        exitCodeNonZero();
        readStdout();
        readStderr();
        writeStdin();
        redirectErrorStream();
        workingDirectory();
        environment();
        missingProgram();
        destroyDoesNotThrow();

        System.out.println();
        System.out.println("passed " + passed + ", failed " + failed
                           + ", skipped " + skipped);
        if (failed > 0) {
            System.exit(1);
        }
    }

    /* ---- checks --------------------------------------------------------- */

    /*
     * The one that used to be impossible: UNIXProcess's static initialiser runs
     * here, so an unsupported-platform Error surfaces on this line and nowhere
     * else.  Caught as Throwable because it is an Error, not an Exception --
     * a catch(Exception) would let it past and blame the next check.
     */
    private static void canLaunch() {
        try {
            Process p = new ProcessBuilder(ECHO, "hello").start();
            p.waitFor();
            pass("launch a process at all", null);
        } catch (Throwable t) {
            fail("launch a process at all", t);
        }
    }

    private static void exitCodeZero() {
        try {
            Process p = new ProcessBuilder(ECHO, "x").start();
            int rc = p.waitFor();
            check("exit code of a command that succeeds", rc == 0, "rc=" + rc);
        } catch (Throwable t) {
            fail("exit code of a command that succeeds", t);
        }
    }

    /*
     * A non-zero code has to survive clib4's child table, which records the raw
     * AmigaDOS return code rather than a POSIX wait status.  If that mapping is
     * wrong every failing child looks successful, which is the kind of bug that
     * hides for months.
     */
    private static void exitCodeNonZero() {
        try {
            /* "quit 5" sets RC=5 on AmigaOS; false(1) gives 1 on Unix. */
            Process p = tryStart("quit", "5");
            if (p == null) {
                p = tryStart("false");
            }
            if (p == null) {
                skip("exit code of a command that fails", "no quit/false command");
                return;
            }
            int rc = p.waitFor();
            check("exit code of a command that fails", rc != 0, "rc=" + rc);
        } catch (Throwable t) {
            fail("exit code of a command that fails", t);
        }
    }

    /* The child's stdout pipe: forkAndExec must hand back the READ end. */
    private static void readStdout() {
        try {
            Process p = new ProcessBuilder(ECHO, "amiga").start();
            String out = drain(p.getInputStream());
            p.waitFor();
            check("read the child's stdout", out.contains("amiga"),
                  "got \"" + out.trim() + "\"");
        } catch (Throwable t) {
            fail("read the child's stdout", t);
        }
    }

    /*
     * Separately from stdout, because the two are different pipes and a wiring
     * mistake that crosses them still passes the stdout check.
     */
    private static void readStderr() {
        try {
            Process p = tryStart("cat", "no-such-file-xyzzy");
            if (p == null) {
                skip("read the child's stderr", "no cat command");
                return;
            }
            String err = drain(p.getErrorStream());
            p.waitFor();
            check("read the child's stderr", err.length() > 0,
                  err.length() + " byte(s)");
        } catch (Throwable t) {
            fail("read the child's stderr", t);
        }
    }

    /* The other direction: the parent's WRITE end of the child's stdin. */
    private static void writeStdin() {
        try {
            Process p = tryStart("cat");
            if (p == null) {
                skip("write to the child's stdin", "no cat command");
                return;
            }
            OutputStream in = p.getOutputStream();
            in.write("ping\n".getBytes("US-ASCII"));
            in.flush();
            in.close();                       /* the child needs to see EOF */
            String out = drain(p.getInputStream());
            p.waitFor();
            check("write to the child's stdin", out.contains("ping"),
                  "echoed back \"" + out.trim() + "\"");
        } catch (Throwable t) {
            fail("write to the child's stdin", t);
        }
    }

    /* With this set the child gets ONE descriptor for both streams. */
    private static void redirectErrorStream() {
        try {
            ProcessBuilder pb = new ProcessBuilder("cat", "no-such-file-xyzzy");
            pb.redirectErrorStream(true);
            Process p;
            try {
                p = pb.start();
            } catch (IOException e) {
                skip("redirectErrorStream", "no cat command");
                return;
            }
            String out = drain(p.getInputStream());
            p.waitFor();
            check("redirectErrorStream merges stderr into stdout",
                  out.length() > 0, out.length() + " byte(s) on stdout");
        } catch (Throwable t) {
            fail("redirectErrorStream", t);
        }
    }

    /* directory() becomes spawnvpe's cwd argument, which it Lock()s. */
    private static void workingDirectory() {
        try {
            File dir = new File(System.getProperty("java.io.tmpdir", "T:"));
            if (!dir.isDirectory()) {
                skip("working directory", dir + " is not a directory");
                return;
            }
            ProcessBuilder pb = new ProcessBuilder(ECHO, "in-cwd");
            pb.directory(dir);
            Process p = pb.start();
            String out = drain(p.getInputStream());
            int rc = p.waitFor();
            check("run with a working directory", rc == 0 && out.contains("in-cwd"),
                  "dir=" + dir + " rc=" + rc);
        } catch (Throwable t) {
            fail("run with a working directory", t);
        }
    }

    /*
     * Note the asymmetry this port has: spawnvpe takes a DELTA environment, so
     * a variable we add is visible to the child, but variables we remove are
     * not actually removed.  The check asserts what is true here -- that what
     * we set arrives -- rather than pretending clear() works.
     */
    private static void environment() {
        try {
            Process p = tryStartEnv("EXECTEST_MARKER", "42");
            if (p == null) {
                skip("environment reaches the child", "no way to read env back");
                return;
            }
            String out = drain(p.getInputStream());
            p.waitFor();
            check("environment reaches the child", out.contains("42"),
                  "got \"" + out.trim() + "\"");
        } catch (Throwable t) {
            fail("environment reaches the child", t);
        }
    }

    /*
     * A program that is not there must come back as IOException.  If the native
     * reported failure some other way this would be an Error or, worse, a
     * process that never exits.
     */
    private static void missingProgram() {
        try {
            new ProcessBuilder("definitely-not-a-real-program-xyzzy").start();
            failed++;
            System.out.println("FAIL missing program should throw: it did not");
        } catch (IOException e) {
            pass("missing program throws IOException", e.getMessage());
        } catch (Throwable t) {
            fail("missing program throws IOException", t);
        }
    }

    /*
     * destroy() is a REQUEST here: clib4's kill sends SIGBREAKF_CTRL_C, the
     * same thing Ctrl-C does from a shell, because an AmigaOS task cannot be
     * torn down from outside.  So this asserts only that it does not throw --
     * claiming the child actually died would be claiming more than the OS gives.
     */
    private static void destroyDoesNotThrow() {
        try {
            Process p = tryStart("cat");          /* waits on stdin forever */
            if (p == null) {
                skip("destroy does not throw", "no cat command");
                return;
            }
            p.destroy();
            pass("destroy does not throw", "(a request, not a kill -- see amiga_process.c)");
            try {
                p.getOutputStream().close();
            } catch (IOException ignored) {
            }
        } catch (Throwable t) {
            fail("destroy does not throw", t);
        }
    }

    /* ---- helpers -------------------------------------------------------- */

    /** Starts a command, or returns null when it is not on this machine. */
    private static Process tryStart(String... cmd) {
        try {
            return new ProcessBuilder(cmd).start();
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * Runs a command that prints one environment variable back.  Tries the
     * shell forms AmigaOS and Unix each understand; null if neither works.
     */
    private static Process tryStartEnv(String name, String value) {
        String[][] attempts = {
            { "sh", "-c", "echo $" + name },
            { "echo", "$" + name },
        };
        for (String[] cmd : attempts) {
            try {
                ProcessBuilder pb = new ProcessBuilder(cmd);
                pb.environment().put(name, value);
                pb.redirectErrorStream(true);
                return pb.start();
            } catch (IOException e) {
                /* try the next form */
            } catch (UnsupportedOperationException e) {
                return null;
            }
        }
        return null;
    }

    /** Reads a stream to EOF.  Bounded so a wiring bug cannot hang the test. */
    private static String drain(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder();
        BufferedReader r = new BufferedReader(new InputStreamReader(in));
        String line;
        int lines = 0;
        while ((line = r.readLine()) != null && lines++ < 100) {
            sb.append(line).append('\n');
        }
        return sb.toString();
    }

    /* ---- reporting ------------------------------------------------------ */

    private static void check(String what, boolean ok, String detail) {
        if (ok) {
            pass(what, detail);
        } else {
            failed++;
            System.out.println("FAIL " + what + ": " + detail);
        }
    }

    private static void pass(String what, String detail) {
        passed++;
        System.out.println("ok   " + what + (detail == null ? "" : ": " + detail));
    }

    private static void skip(String what, String why) {
        skipped++;
        System.out.println("skip " + what + ": " + why);
    }

    private static void fail(String what, Throwable t) {
        failed++;
        System.out.println("FAIL " + what + ": " + t);
        t.printStackTrace(System.out);
    }
}
