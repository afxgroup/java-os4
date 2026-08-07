/*
 * ExecLeak -- run exactly N exec()s and pause, so the leftover PIPE: handles
 * can be counted while the JVM is still alive.
 *
 * ExecTest leaves around thirty PIPE: handles open.  Thirty is 3 x 10, and
 * ExecTest launches ten processes, which is suggestive but not proof: three per
 * process is also how many handles spawnvpe duplicates for the child
 * (SYS_Input, SYS_Output, SYS_Error), and six per process is what you would see
 * if the parent's ends were leaking too.  This tells the two apart.
 *
 *     java -cp examples/ExecLeak.jar ExecLeak [count]
 *
 * PIPE: cannot be listed -- it is a read/write device with no directory -- so
 * the shell has no way to count pipe handles.  What this CAN count by itself is
 * its own open descriptors, and that is the discriminating half: the ends the
 * parent keeps are ours, the ends handed to a child are not, since spawnvpe
 * duplicates those and the child owns them afterwards.
 *
 * So read the "open fds" line this prints, and if you have a process monitor
 * (Scout, SysMon) look at the PIPE: handler alongside it.  The pauses are there
 * so both can be read while the JVM is still alive -- once it exits everything
 * goes with the process.
 *
 * Reading the answer, with D = (after - before):
 *
 *     D == 0            one exec leaks nothing; something specific to one of
 *                       ExecTest's ten cases is responsible, and running them
 *                       one at a time will name it.
 *     open fds +6/exec  the parent's ends: UNIXProcess is not closing what
 *                       forkAndExec handed it, and that is ours to fix.
 *     open fds flat,    the child's ends.  spawnvpe DupFileHandle()s three
 *     handlers grow     handles per child and expects SystemTags with
 *                       SYS_Asynch to close them; that would be the clib4 side.
 *
 * Run it twice, with 1 and with 3, to confirm the delta scales per exec rather
 * than being a fixed cost paid once.
 *
 * GPLv2 (java-os4 project).
 */
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;

public class ExecLeak {

    public static void main(String[] args) throws Exception {
        int count = 1;
        if (args.length > 0) {
            try {
                count = Integer.parseInt(args[0]);
            } catch (NumberFormatException e) {
                System.out.println("usage: ExecLeak [count]   (default 1)");
                System.exit(2);
            }
        }

        System.out.println("ExecLeak -- " + count + " exec(s) on "
                           + System.getProperty("os.name"));
        System.out.println();

        pause("BEFORE");

        int ok = 0;
        for (int i = 0; i < count; i++) {
            if (runOne(i + 1)) {
                ok++;
            }
        }

        System.out.println();
        System.out.println(ok + " of " + count + " exec(s) completed");
        if (ok != count) {
            /* A failed exec opens and closes its pipes on the error path, which
               is a different question from the one being asked.  Say so rather
               than let it quietly spoil the arithmetic. */
            System.out.println("WARNING: not every exec ran -- the delta below "
                               + "does not answer the question cleanly");
        }

        /* The reaper closes the streams from another thread, so give it a
           moment before counting or the answer is a race. */
        Thread.sleep(1000);

        pause("AFTER " + count + " exec(s) -- the difference is the answer");

        /* Only now, in case anything is waiting on a finaliser rather than on
           an explicit close.  If the count drops here, the handles were not
           leaked so much as released late, which is a different fix. */
        System.gc();
        Thread.sleep(1000);

        pause("AFTER GC -- a count that falls here was released late, not leaked");

        System.out.println();
        System.out.println("done -- the handles go away with this process now");
    }

    /*
     * One exec with all three pipes: no redirect to a file, no
     * redirectErrorStream, so forkAndExec creates the full set.  Everything is
     * read and closed the way a well-behaved caller would, because the question
     * is what leaks when NOTHING is done wrong.
     */
    private static boolean runOne(int n) {
        Process p = null;
        try {
            p = new ProcessBuilder("echo", "leak-probe-" + n).start();

            String out = drain(p.getInputStream());
            String err = drain(p.getErrorStream());
            p.getOutputStream().close();

            int rc = p.waitFor();
            System.out.println("  exec " + n + ": rc=" + rc
                               + " stdout=\"" + out.trim() + "\""
                               + (err.length() > 0 ? " stderr=" + err.length() + "b" : ""));
            return true;
        } catch (IOException e) {
            System.out.println("  exec " + n + ": FAILED " + e);
            return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } finally {
            /* Belt and braces: closing an already-closed stream is harmless,
               and leaving one open would be us causing the leak we are hunting. */
            if (p != null) {
                closeQuietly(p.getInputStream());
                closeQuietly(p.getErrorStream());
                closeQuietly(p.getOutputStream());
            }
        }
    }

    private static String drain(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder();
        BufferedReader r = new BufferedReader(new InputStreamReader(in));
        String line;
        int lines = 0;
        while ((line = r.readLine()) != null && lines++ < 50) {
            sb.append(line);
        }
        return sb.toString();
    }

    /*
     * Waits for Enter so the count can be taken by hand.  Falls back to a timed
     * pause when there is no console to read from, so the test still works
     * unattended (redirected input, a script).
     */
    private static void pause(String what) throws Exception {
        System.out.println();
        System.out.println(">>> " + what + "  |  open fds: " + openFds());
        System.out.print(">>> press Enter to continue... ");
        System.out.flush();

        int c = System.in.read();
        if (c < 0) {
            System.out.println("(no console -- waiting 20s instead)");
            Thread.sleep(20000);
        } else {
            /* Swallow the rest of the line so the next pause does not return
               immediately on the leftover newline. */
            while (c != '\n' && c >= 0) {
                c = System.in.read();
            }
            System.out.println();
        }
    }

    /*
     * Reflection so the same jar still runs on a host JDK, which has no
     * AmigaDiag.  A test that only works where the bug is cannot be checked
     * against a platform where the answer is known.
     */
    private static String openFds() {
        try {
            Class<?> diag = Class.forName("java.lang.AmigaDiag");
            Object n = diag.getMethod("openFdCount").invoke(null);
            return String.valueOf(n);
        } catch (Throwable t) {
            return "n/a (not this runtime)";
        }
    }

    private static void closeQuietly(java.io.Closeable c) {
        if (c != null) {
            try {
                c.close();
            } catch (IOException ignored) {
            }
        }
    }
}
