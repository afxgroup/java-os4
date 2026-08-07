/*
 * NetDownload -- fetch a URL and show throughput, progress and ETA.
 *
 * The point is to watch the network while it works: how fast it is going right
 * now, how much is left, how long that will take.
 *
 *     java -cp examples/NetDownload.jar NetDownload <url> [outfile]
 *     java -cp examples/NetDownload.jar NetDownload <url> -
 *
 * With no outfile the name is taken from the URL.  With "-" the body is read
 * and thrown away, which measures the NETWORK on its own -- worth having,
 * because on an Amiga the disk is often the slower half and would otherwise be
 * what you are really timing.
 *
 * Works over http and https alike (https needs the SunEC provider, which
 * NetTest checks).  Redirects are followed by hand, including http -> https:
 * HttpURLConnection refuses to follow those itself, and a silent stop at the
 * redirect is a confusing way to find that out.
 *
 * GPLv2 (java-os4 project).
 */
import java.io.BufferedOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;

public class NetDownload {

    /*
     * 64K because that is the largest read the native layer will actually
     * perform for us: SocketInputStream.socketRead0 clamps len to
     * MAX_HEAP_BUFFER_LEN, which on a 32-bit JDK -- ours -- is 65536 (the
     * 131072 next to it in net_util_md.h is the _LP64 branch).  Asking for more
     * just wastes an array and still reads 64K at a time.
     *
     * Below 8192 the native side would use its stack buffer instead of a
     * malloc, which sounds better and is not: dropping to 8K quadruples the
     * recv() count, and measurement says the syscalls dominate by a wide margin
     * (40MB over loopback: 1285 recv at 32K vs 5286 at 8K, ~4x the wall time).
     * One allocation per 64K is the cheaper end of that trade.
     */
    private static final int BUFFER = 64 * 1024;
    private static final int MAX_REDIRECTS = 5;
    /* Redraw at most this often: the progress line goes to a console that may
     * be a serial port, where writing on every 32K chunk would itself cost
     * more than the download. */
    private static final long REDRAW_MS = 500;

    public static void main(String[] args) {
        if (args.length < 1 || args.length > 2) {
            System.out.println("usage: NetDownload <url> [outfile|-]");
            System.out.println("       -  read and discard (measures the network only)");
            System.exit(2);
        }

        String url = args[0];
        String out = args.length > 1 ? args[1] : nameFromUrl(url);
        boolean discard = "-".equals(out);

        try {
            download(url, out, discard);
        } catch (Exception e) {
            System.out.println();
            System.out.println("failed: " + e);
            System.exit(1);
        }
    }

    private static void download(String url, String out, boolean discard)
            throws IOException {

        URLConnection conn = open(url);
        long total = contentLength(conn);

        System.out.println("url   : " + conn.getURL());
        System.out.println("size  : " + (total < 0 ? "unknown" : bytes(total)));
        System.out.println("out   : " + (discard ? "(discarded)" : out));
        System.out.println();

        InputStream in = conn.getInputStream();
        OutputStream sink = discard ? null
                : new BufferedOutputStream(new FileOutputStream(out), BUFFER);

        byte[] buf = new byte[BUFFER];
        long done = 0;
        long start = System.currentTimeMillis();
        long lastDraw = start;
        /* Speed is sampled over the gap between redraws rather than averaged
         * from the start, so the figure reflects the line right now -- which is
         * the one you watch to tell a stall from a slow server. */
        long windowStart = start;
        long windowBytes = 0;
        double speed = 0;

        try {
            int n;
            while ((n = in.read(buf)) > 0) {
                if (sink != null) {
                    sink.write(buf, 0, n);
                }
                done += n;
                windowBytes += n;

                long now = System.currentTimeMillis();
                if (now - lastDraw >= REDRAW_MS) {
                    long span = now - windowStart;
                    if (span > 0) {
                        speed = windowBytes * 1000.0 / span;
                    }
                    windowStart = now;
                    windowBytes = 0;
                    lastDraw = now;
                    draw(done, total, speed);
                }
            }
        } finally {
            if (sink != null) {
                sink.close();
            }
            in.close();
        }

        long elapsed = Math.max(1, System.currentTimeMillis() - start);
        draw(done, total < 0 ? done : total, done * 1000.0 / elapsed);
        System.out.println();
        System.out.println();
        System.out.println("got   : " + bytes(done) + " in " + seconds(elapsed / 1000)
                           + "  (avg " + bytes((long)(done * 1000.0 / elapsed)) + "/s)");

        /* A short read looks like success until you open the file: the stream
         * just ends.  Say so. */
        if (total >= 0 && done != total) {
            System.out.println("WARNING: expected " + bytes(total)
                               + ", the connection ended after " + bytes(done));
            System.exit(1);
        }
    }

    /* ---- connection ---------------------------------------------------- */

    private static URLConnection open(String url) throws IOException {
        for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
            URLConnection c = new URL(url).openConnection();
            c.setConnectTimeout(20000);
            /*
             * No read timeout, deliberately.  A non-zero SO_TIMEOUT sends every
             * single read down SocketInputStream's NET_ReadWithTimeout path --
             * poll() followed by recv(MSG_DONTWAIT) -- instead of one plain
             * blocking recv(), doubling the socket calls for the whole
             * transfer.  Over loopback that alone was a fifth of the
             * throughput, and bsdsocket calls are not cheaper here.
             *
             * What we give up is the automatic abort on a dead connection.  For
             * this tool that is a fair trade: the progress line redraws twice a
             * second, so a stall is visible the moment it happens and Ctrl-C is
             * right there.  The connect timeout stays -- without it an
             * unreachable host hangs before printing anything at all.
             */

            if (!(c instanceof HttpURLConnection)) {
                c.connect();
                return c;
            }

            HttpURLConnection h = (HttpURLConnection) c;
            /* Off, so a cross-protocol hop reaches the code below instead of
             * being silently dropped. */
            h.setInstanceFollowRedirects(false);
            h.setRequestProperty("User-Agent", "Java-OS4 NetDownload");
            h.connect();

            int code = h.getResponseCode();
            if (code / 100 == 3) {
                String next = h.getHeaderField("Location");
                h.disconnect();
                if (next == null) {
                    throw new IOException("HTTP " + code + " without a Location header");
                }
                /* Location may be relative. */
                url = new URL(new URL(url), next).toString();
                System.out.println("  -> " + code + " redirect to " + url);
                continue;
            }
            if (code / 100 != 2) {
                String msg = h.getResponseMessage();
                h.disconnect();
                throw new IOException("HTTP " + code + (msg == null ? "" : " " + msg));
            }
            return h;
        }
        throw new IOException("too many redirects (more than " + MAX_REDIRECTS + ")");
    }

    /* getContentLength() is an int, so it overflows above 2GB. */
    private static long contentLength(URLConnection c) {
        String s = c.getHeaderField("Content-Length");
        if (s == null) {
            return -1;
        }
        try {
            long v = Long.parseLong(s.trim());
            return v >= 0 ? v : -1;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static String nameFromUrl(String url) {
        String path = url;
        int q = path.indexOf('?');
        if (q >= 0) {
            path = path.substring(0, q);
        }
        int slash = path.lastIndexOf('/');
        if (slash >= 0 && slash < path.length() - 1) {
            path = path.substring(slash + 1);
        } else {
            path = "";
        }
        return path.length() == 0 ? "download.out" : path;
    }

    /* ---- progress line -------------------------------------------------- */

    /*
     * One line, rewritten in place with '\r'.  Kept under 79 columns so it does
     * not wrap on a standard Amiga console -- a wrapped line turns the whole
     * download into a scrolling wall instead of a single updating row.
     */
    private static void draw(long done, long total, double speed) {
        StringBuilder sb = new StringBuilder(80);
        sb.append('\r');

        if (total > 0) {
            int pct = (int)(done * 100 / total);
            sb.append('[');
            int width = 24;
            int full = (int)(done * width / total);
            for (int i = 0; i < width; i++) {
                sb.append(i < full ? '=' : (i == full ? '>' : ' '));
            }
            sb.append("] ");
            sb.append(pad(pct + "%", 4));
        } else {
            sb.append(pad("", 31));
        }

        sb.append(' ').append(pad(bytes(done), 9));
        sb.append(' ').append(pad(bytes((long) speed) + "/s", 11));

        if (total > 0 && speed > 1) {
            long left = (long)((total - done) / speed);
            sb.append(" eta ").append(seconds(left));
        }

        /* Trailing blanks wipe whatever the previous, longer line left behind. */
        sb.append("      ");
        System.out.print(sb);
        System.out.flush();
    }

    private static String bytes(long n) {
        if (n < 1024) {
            return n + " B";
        }
        if (n < 1024L * 1024) {
            return round(n / 1024.0) + " KB";
        }
        if (n < 1024L * 1024 * 1024) {
            return round(n / (1024.0 * 1024)) + " MB";
        }
        return round(n / (1024.0 * 1024 * 1024)) + " GB";
    }

    /* One decimal, without String.format -- it drags in the whole formatter. */
    private static String round(double v) {
        long tenths = (long)(v * 10 + 0.5);
        return (tenths / 10) + "." + (tenths % 10);
    }

    private static String seconds(long s) {
        if (s < 60) {
            return s + "s";
        }
        if (s < 3600) {
            return (s / 60) + "m" + two(s % 60) + "s";
        }
        return (s / 3600) + "h" + two((s % 3600) / 60) + "m";
    }

    private static String two(long v) {
        return v < 10 ? "0" + v : Long.toString(v);
    }

    private static String pad(String s, int width) {
        StringBuilder sb = new StringBuilder(width);
        for (int i = s.length(); i < width; i++) {
            sb.append(' ');
        }
        return sb.append(s).toString();
    }
}
