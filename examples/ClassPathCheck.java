/*
 * ClassPathCheck -- can the runtime actually read what is on the class path?
 *
 *     java -cp examples/ClassPathCheck.jar ClassPathCheck [class.to.find]
 *
 * A NoClassDefFoundError names the class that was wanted, never the reason it
 * was not there, and on this port the candidate reasons are quite different
 * from each other:
 *
 *   - the entry is in java.class.path but the file is not on disk,
 *   - the file is there but the runtime cannot open it (a path that survived
 *     being printed but not being converted),
 *   - it opens but is not a readable zip (a truncated download),
 *   - it is all fine and the class genuinely is not in any of them,
 *   - the class is there and loading it fails for a reason of its OWN -- a
 *     missing dependency inside a static initialiser, which surfaces as a
 *     NoClassDefFoundError naming the outer class and blaming the wrong jar.
 *
 * Those need fixing in five different places, so this walks the class path and
 * says which one it is.  With a class name it also reports which entry holds
 * that class, then tries to load it and prints what actually went wrong.
 *
 * Written for InvoiceX, whose startup fails with
 * "java.lang.NoClassDefFoundError: it/tnx/Db" while lib/commons-tnx.jar is
 * present in java.class.path -- so:
 *
 *     java -cp examples/ClassPathCheck.jar:Invoicex.jar:lib/* ClassPathCheck it.tnx.Db
 *
 * GPLv2 (java-os4 project).
 */
import java.io.File;
import java.io.IOException;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class ClassPathCheck {

    public static void main(String[] args) {
        String wanted = args.length > 0 ? args[0] : null;

        String cp = System.getProperty("java.class.path");
        String sep = System.getProperty("path.separator");

        System.out.println("path.separator : \"" + sep + "\"");
        System.out.println("user.dir       : " + System.getProperty("user.dir"));
        System.out.println();

        if (cp == null || cp.length() == 0) {
            System.out.println("java.class.path is empty -- nothing to check.");
            return;
        }

        String[] entries = cp.split(java.util.regex.Pattern.quote(sep));
        System.out.println(entries.length + " class path entries");
        System.out.println();

        String wantedPath = wanted == null ? null : wanted.replace('.', '/') + ".class";
        int missing = 0, unreadable = 0, broken = 0, ok = 0;
        String foundIn = null;

        for (String e : entries) {
            if (e.length() == 0) {
                continue;
            }
            File f = new File(e);
            String status;

            if (!f.exists()) {
                /* The most likely one on this port: an entry that reads
                   correctly but names a place the runtime cannot reach. */
                status = "MISSING   (not on disk)";
                missing++;
            } else if (!f.canRead()) {
                status = "UNREADABLE";
                unreadable++;
            } else if (f.isDirectory()) {
                status = "dir";
                ok++;
                if (wantedPath != null && new File(f, wantedPath).exists()) {
                    foundIn = e;
                }
            } else {
                ZipFile z = null;
                try {
                    z = new ZipFile(f);
                    int n = 0;
                    for (Enumeration<? extends ZipEntry> en = z.entries();
                         en.hasMoreElements(); en.nextElement()) {
                        n++;
                    }
                    status = "ok        (" + n + " entries, " + f.length() + " bytes)";
                    ok++;
                    if (wantedPath != null && z.getEntry(wantedPath) != null) {
                        foundIn = e;
                    }
                } catch (IOException ex) {
                    /* A truncated or half-written jar lands here, which is
                       exactly what a failed self-update leaves behind. */
                    status = "NOT A ZIP (" + ex + ")";
                    broken++;
                } finally {
                    if (z != null) {
                        try {
                            z.close();
                        } catch (IOException ignored) {
                            /* nothing useful to do; the report is what matters */
                        }
                    }
                }
            }
            System.out.println("  " + pad(status, 42) + shorten(e));
        }

        System.out.println();
        System.out.println("summary: " + ok + " ok, " + missing + " missing, "
                           + unreadable + " unreadable, " + broken + " not a zip");

        if (wanted == null) {
            System.out.println();
            System.out.println("pass a class name to also check where it lives, e.g.");
            System.out.println("  ClassPathCheck it.tnx.Db");
            return;
        }

        System.out.println();
        System.out.println("looking for " + wanted);
        System.out.println("  as resource : " + (foundIn == null ? "NOT in any entry" : foundIn));

        /*
         * Loading is asked separately from finding, because the two failures
         * mean opposite things.  Not found: the jar is missing or wrong.  Found
         * but will not load: the jar is fine and something the class needs is
         * not -- and the error Java reports in that case names this class, which
         * is what sends people to look in the wrong jar.
         */
        try {
            Class<?> c = Class.forName(wanted);
            System.out.println("  Class.forName: ok, loaded by " + c.getClassLoader());
        } catch (ClassNotFoundException ex) {
            System.out.println("  Class.forName: ClassNotFoundException"
                               + (foundIn != null
                                  ? "  <-- but the .class IS in " + shorten(foundIn)
                                    + ", so the entry is not being searched"
                                  : ""));
        } catch (Throwable t) {
            /* NoClassDefFoundError, ExceptionInInitializerError, LinkageError:
               the class was reached and its own initialisation failed.  The
               cause is the useful half and is what the original stack trace
               left out. */
            System.out.println("  Class.forName: " + t);
            Throwable cause = t.getCause();
            while (cause != null) {
                System.out.println("        caused by: " + cause);
                cause = cause.getCause();
            }
            System.out.println("  -- the jar is fine; something this class NEEDS is not.");
        }
    }

    /* Class paths on this platform are long and absolute; the tail is the part
       that differs between entries. */
    private static String shorten(String s) {
        return s.length() <= 58 ? s : "..." + s.substring(s.length() - 55);
    }

    private static String pad(String s, int w) {
        StringBuilder sb = new StringBuilder(s);
        while (sb.length() < w) {
            sb.append(' ');
        }
        return sb.toString();
    }
}
