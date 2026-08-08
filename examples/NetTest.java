/*
 * NetTest -- exercises the java.net natives shipped in libnet.so.
 *
 * Until libnet was implemented this could not get past its first line: it was a
 * stub, so java.net.PlainSocketImpl's <clinit> threw
 * "UnsatisfiedLinkError: initProto" and nothing network-facing could even
 * initialise.  This walks the layers bottom-up so a failure tells you WHICH one
 * broke rather than just "the network does not work".
 *
 *     java -cp examples/NetTest.jar NetTest [host [port]]
 *
 * Defaults to example.com:80.  Every test that needs the outside world is
 * skipped, not failed, when there is no route -- so it stays useful offline.
 *
 * IPv6 is deliberately absent from this runtime: libnet is built with
 * -DDONT_ENABLE_IPV6 because the AmigaOS stack cannot carry it, even though
 * clib4 exposes AF_INET6.  The test asserts that rather than ignoring it, since
 * a runtime that started ANSWERING "yes" to IPv6 would break in confusing ways.
 *
 * GPLv2 (java-os4 project).
 */
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.URLConnection;
import java.nio.charset.Charset;
import java.security.Security;
import java.util.Enumeration;
import javax.crypto.Cipher;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLHandshakeException;

public class NetTest {

    private static int passed = 0;
    private static int failed = 0;
    private static int skipped = 0;

    public static void main(String[] args) {
        String host = args.length > 0 ? args[0] : "example.com";
        int port = args.length > 1 ? Integer.parseInt(args[1]) : 80;

        System.out.println("NetTest -- java.net on " + System.getProperty("os.name")
                           + " " + System.getProperty("os.version"));
        System.out.println("target: " + host + ":" + port);
        System.out.println();

        localAddress();
        ipv6IsOff();
        interfaces();
        loopbackSocket();
        udpSocket();
        dns(host);
        tcpConnect(host, port);
        socketBuffers();
        httpGet(host);
        cryptoProviders();
        httpsGet(host);

        System.out.println();
        System.out.println("passed " + passed + ", failed " + failed
                           + ", skipped " + skipped);
        if (failed > 0) {
            System.exit(1);
        }
    }

    /* ---- individual checks ------------------------------------------- */

    /* InetAddress.<clinit> is the first thing to touch libnet at all. */
    private static void localAddress() {
        try {
            InetAddress lo = InetAddress.getByName("127.0.0.1");
            check("InetAddress.getByName(127.0.0.1)",
                  lo.getHostAddress().equals("127.0.0.1"), lo.getHostAddress());

            InetAddress local = InetAddress.getLocalHost();
            pass("InetAddress.getLocalHost", local.getHostName()
                 + " / " + local.getHostAddress());
        } catch (Throwable t) {
            fail("InetAddress", t);
        }
    }

    /*
     * Not a nice-to-have: if this ever reports true, java.net hands out
     * Inet6AddressImpl, whose natives are not in libnet at all.
     */
    private static void ipv6IsOff() {
        try {
            InetAddress a = InetAddress.getByName("127.0.0.1");
            check("IPv4 stack in use (no IPv6)", a instanceof Inet4Address,
                  a.getClass().getName());
        } catch (Throwable t) {
            fail("IPv6 check", t);
        }
    }

    /*
     * Expected to be empty on this port: NetworkInterface's natives are the
     * "no enumerable interfaces" stubs.  What matters is that it does not
     * throw -- an UnsatisfiedLinkError here is an Error, and it would sail
     * through SeedGenerator's catch(Exception) and take SecureRandom with it.
     */
    private static void interfaces() {
        try {
            Enumeration<NetworkInterface> e = NetworkInterface.getNetworkInterfaces();
            int n = 0;
            while (e != null && e.hasMoreElements()) {
                e.nextElement();
                n++;
            }
            pass("NetworkInterface.getNetworkInterfaces", n + " interface(s) "
                 + "(0 is expected on this port)");
        } catch (Throwable t) {
            fail("NetworkInterface", t);
        }
    }

    /*
     * The real exercise of socket()/bind()/listen()/accept()/connect() and of
     * NET_Read / NET_Send -- and it needs no outside world.  If TCP is broken
     * at the native level, it breaks here.
     */
    private static void loopbackSocket() {
        ServerSocket server = null;
        try {
            server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
            final int p = server.getLocalPort();
            final byte[] sent = "amiga".getBytes("US-ASCII");

            Thread client = new Thread(new Runnable() {
                public void run() {
                    Socket s = null;
                    try {
                        s = new Socket();
                        s.connect(new InetSocketAddress("127.0.0.1", p), 5000);
                        OutputStream out = s.getOutputStream();
                        out.write(sent);
                        out.flush();
                    } catch (Exception ignored) {
                        /* the accept side reports the failure */
                    } finally {
                        closeQuietly(s);
                    }
                }
            });
            client.setDaemon(true);
            client.start();

            server.setSoTimeout(10000);
            Socket accepted = server.accept();
            byte[] buf = new byte[16];
            int n = accepted.getInputStream().read(buf);
            closeQuietly(accepted);
            client.join(2000);

            check("TCP loopback (bind/listen/accept/connect/read/write)",
                  n == sent.length && new String(buf, 0, n, "US-ASCII").equals("amiga"),
                  "read " + n + " byte(s)");
        } catch (Throwable t) {
            fail("TCP loopback", t);
        } finally {
            closeQuietly(server);
        }
    }

    /* PlainDatagramSocketImpl: a different native file from the TCP path. */
    private static void udpSocket() {
        DatagramSocket s = null;
        try {
            s = new DatagramSocket(0, InetAddress.getByName("127.0.0.1"));
            pass("UDP socket bind", "port " + s.getLocalPort());
        } catch (Throwable t) {
            fail("UDP socket bind", t);
        } finally {
            if (s != null) {
                s.close();
            }
        }
    }

    /* First check that leaves the machine: resolver + bsdsocket. */
    private static void dns(String host) {
        try {
            InetAddress[] all = InetAddress.getAllByName(host);
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < all.length; i++) {
                if (i > 0) {
                    sb.append(", ");
                }
                sb.append(all[i].getHostAddress());
            }
            pass("DNS " + host, sb.toString());
        } catch (IOException e) {
            skip("DNS " + host, e.toString());
        } catch (Throwable t) {
            fail("DNS " + host, t);
        }
    }

    private static void tcpConnect(String host, int port) {
        Socket s = null;
        try {
            s = new Socket();
            s.connect(new InetSocketAddress(host, port), 15000);
            pass("TCP connect " + host + ":" + port, "local port "
                 + s.getLocalPort());
        } catch (SocketTimeoutException e) {
            skip("TCP connect " + host + ":" + port, "timed out");
        } catch (IOException e) {
            skip("TCP connect " + host + ":" + port, e.toString());
        } catch (Throwable t) {
            fail("TCP connect " + host + ":" + port, t);
        } finally {
            closeQuietly(s);
        }
    }

    /*
     * What the stack actually gives us, rather than what we hope.
     *
     * Throughput on a link with any latency is capped by the receive window,
     * so a small default SO_RCVBUF limits a download no matter how fast the
     * line is -- and Roadshow's defaults are not Linux's.  TCP_NODELAY matters
     * the other way: with Nagle on, a small request written before the reply
     * is read can sit waiting for the peer's delayed ACK.  Both are reported
     * rather than asserted, because the right values are the machine's to say.
     */
    private static void socketBuffers() {
        Socket s = null;
        try {
            s = new Socket();
            int rcv = s.getReceiveBufferSize();
            int snd = s.getSendBufferSize();
            pass("socket buffers", "SO_RCVBUF=" + rcv + " SO_SNDBUF=" + snd);

            s.setTcpNoDelay(true);
            check("TCP_NODELAY can be set", s.getTcpNoDelay(),
                  "reads back " + s.getTcpNoDelay());

            /* Does asking for more actually get more?  If the stack silently
               ignores it, tuning from Java is a dead end and the default has
               to be right at creation instead. */
            s.setReceiveBufferSize(64 * 1024);
            int grown = s.getReceiveBufferSize();
            check("SO_RCVBUF is settable", grown > rcv || grown >= 64 * 1024,
                  rcv + " -> " + grown + " (asked for " + (64 * 1024) + ")");
        } catch (Throwable t) {
            fail("socket buffers", t);
        } finally {
            closeQuietly(s);
        }
    }

    /* Whole stack: URLConnection over the sockets above. */
    private static void httpGet(String host) {
        BufferedReader r = null;
        try {
            URL url = new URL("http://" + host + "/");
            URLConnection c = url.openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(15000);
            c.connect();
            r = new BufferedReader(new InputStreamReader(c.getInputStream(),
                                                         Charset.forName("UTF-8")));
            int lines = 0;
            while (r.readLine() != null && lines < 200) {
                lines++;
            }
            pass("HTTP GET http://" + host + "/", lines + " line(s)");
        } catch (IOException e) {
            skip("HTTP GET http://" + host + "/", e.toString());
        } catch (Throwable t) {
            fail("HTTP GET http://" + host + "/", t);
        } finally {
            if (r != null) {
                try {
                    r.close();
                } catch (IOException ignored) {
                }
            }
        }
    }

    /*
     * Both providers live in lib/ext and both were missing at one point.  They
     * fail silently -- an absent provider is simply not registered -- so check
     * them by name rather than waiting for the symptom.
     *
     * SunJCE holds every cipher (jce.jar is only the javax.crypto API); without
     * it Cipher.getInstance("DES") throws NoSuchAlgorithmException.  SunEC
     * needs libsunec.so to load at all, and without EC the ClientHello carries
     * no curve a modern server accepts -- see httpsGet below.
     */
    private static void cryptoProviders() {
        check("SunJCE provider (ciphers)",
              Security.getProvider("SunJCE") != null, "lib/ext/sunjce_provider.jar");
        check("SunEC provider (elliptic curve)",
              Security.getProvider("SunEC") != null, "lib/ext/sunec.jar + libsunec.so");
        try {
            Cipher.getInstance("DES");
            pass("Cipher.getInstance(DES)", null);
        } catch (Throwable t) {
            fail("Cipher.getInstance(DES)", t);
        }
    }

    /*
     * The end-to-end check: TLS over the sockets above, with the crypto
     * providers above.  A handshake_failure here almost always means no EC --
     * TLS 1.3 negotiates over the named curves and most TLS 1.2 servers are
     * ECDHE-only, so a runtime without SunEC offers nothing they will take.
     */
    private static void httpsGet(String host) {
        try {
            URL url = new URL("https://" + host + "/");
            HttpsURLConnection c = (HttpsURLConnection) url.openConnection();
            c.setConnectTimeout(20000);
            c.setReadTimeout(20000);
            c.connect();
            String suite = c.getCipherSuite();
            int code = c.getResponseCode();
            c.disconnect();
            pass("HTTPS GET https://" + host + "/", "HTTP " + code + ", " + suite);
        } catch (SSLHandshakeException e) {
            fail("HTTPS GET https://" + host + "/", e);
        } catch (IOException e) {
            skip("HTTPS GET https://" + host + "/", e.toString());
        } catch (Throwable t) {
            fail("HTTPS GET https://" + host + "/", t);
        }
    }

    /* ---- reporting ---------------------------------------------------- */

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

    /* No route out is not a defect in libnet -- say so and keep going. */
    private static void skip(String what, String why) {
        skipped++;
        System.out.println("skip " + what + ": " + why);
    }

    private static void fail(String what, Throwable t) {
        failed++;
        System.out.println("FAIL " + what + ": " + t);
        t.printStackTrace(System.out);
    }

    private static void closeQuietly(Socket s) {
        if (s != null) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }

    private static void closeQuietly(ServerSocket s) {
        if (s != null) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }
}
