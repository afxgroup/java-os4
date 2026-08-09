/*
 * LambdaTest -- does invokedynamic work on this runtime?
 *
 *     java -cp examples/LambdaTest.jar LambdaTest
 *
 * Every lambda and method reference in Java 8 compiles to an `invokedynamic`
 * bytecode, resolved at first execution through java.lang.invoke -- MethodType,
 * MethodHandle, LambdaMetafactory.  That machinery is the least exercised
 * corner of JamVM (classlib/openjdk/mh.c), and a modern application is full of
 * it whether or not its author thinks about lambdas: every Comparator.comparing,
 * every stream, every Runnable written with an arrow.
 *
 * This exists because InvoiceX crashed with a DSI inside
 *
 *     executeJava (OPC_INVOKEINTERFACE_QUICK)
 *       <- executeMethodArgs
 *       <- findMethodHandleType    mh.c:712
 *       <- resolveMethodHandle     mh.c:948
 *       <- findInvokeDynamicInvoker mh.c:1054
 *
 * i.e. while resolving an invokedynamic call site.  A twelve-megabyte
 * application is a poor place to debug that from; this is the same machinery in
 * a form that starts in a second.
 *
 * Each case is run separately and reports on its own line, because the useful
 * result is not "it crashed" but WHICH shape crashed -- a plain lambda, a
 * method reference, a captured variable and a generic signature reach different
 * paths through MethodType resolution.
 *
 * GPLv2 (java-os4 project).
 */
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;

public class LambdaTest {

    private static int passed, failed;

    public static void main(String[] args) {
        System.out.println("LambdaTest -- invokedynamic / java.lang.invoke");
        System.out.println("java.version: " + System.getProperty("java.version"));
        System.out.println();

        /* Ordered by how much of java.lang.invoke each one needs, so the last
           line printed says how far the runtime got. */

        check("Runnable, no capture", new Runnable() {
            public void run() {
                Runnable r = () -> { };
                r.run();
            }
        });

        check("Supplier returning a constant", new Runnable() {
            public void run() {
                Supplier<String> s = () -> "ok";
                require("ok".equals(s.get()), "wrong value");
            }
        });

        check("lambda capturing a local", new Runnable() {
            public void run() {
                int captured = 41;
                Supplier<Integer> s = () -> captured + 1;
                require(s.get() == 42, "wrong value");
            }
        });

        check("Function with a generic signature", new Runnable() {
            public void run() {
                Function<String, Integer> f = str -> str.length();
                require(f.apply("abcd") == 4, "wrong length");
            }
        });

        check("BiFunction, two arguments", new Runnable() {
            public void run() {
                BiFunction<Integer, Integer, Integer> add = (a, b) -> a + b;
                require(add.apply(2, 3) == 5, "wrong sum");
            }
        });

        check("method reference to a static", new Runnable() {
            public void run() {
                Function<String, Integer> f = Integer::parseInt;
                require(f.apply("123") == 123, "wrong value");
            }
        });

        check("method reference to an instance method", new Runnable() {
            public void run() {
                Function<String, String> f = String::trim;
                require("x".equals(f.apply("  x  ")), "wrong value");
            }
        });

        check("constructor reference", new Runnable() {
            public void run() {
                Supplier<ArrayList<String>> f = ArrayList::new;
                require(f.get() != null, "null list");
            }
        });

        check("Predicate, and a captured object", new Runnable() {
            public void run() {
                String prefix = "ab";
                Predicate<String> p = str -> str.startsWith(prefix);
                require(p.test("abc") && !p.test("xyz"), "wrong result");
            }
        });

        /* Comparator.comparing is where real applications spend their
           invokedynamic: a method reference handed to a generic factory that
           itself returns a lambda. */
        check("Comparator.comparing (lambda inside a lambda)", new Runnable() {
            public void run() {
                List<String> l = new ArrayList<String>(
                    Arrays.asList("ccc", "a", "bb"));
                l.sort(Comparator.comparing(String::length));
                require("a".equals(l.get(0)) && "ccc".equals(l.get(2)),
                        "wrong order: " + l);
            }
        });

        check("Callable through a checked-exception signature", new Runnable() {
            public void run() {
                Callable<String> c = () -> "called";
                try {
                    require("called".equals(c.call()), "wrong value");
                } catch (Exception e) {
                    require(false, "threw " + e);
                }
            }
        });

        /* The same call site executed many times: resolution happens once and
           the rest go through the resolved path, which is a different code
           path from the first. */
        check("one call site, 10000 executions", new Runnable() {
            public void run() {
                int total = 0;
                for (int i = 0; i < 10000; i++) {
                    Supplier<Integer> s = () -> 1;
                    total += s.get();
                }
                require(total == 10000, "wrong total " + total);
            }
        });

        System.out.println();
        System.out.println(passed + " passed, " + failed + " failed");
        if (failed > 0) {
            System.exit(1);
        }
    }

    private static void check(String what, Runnable body) {
        /* Printed BEFORE running, and flushed: if the case takes the VM down
           there is no line afterwards to say which one it was. */
        System.out.print("  " + pad(what, 46));
        System.out.flush();
        try {
            body.run();
            System.out.println("ok");
            passed++;
        } catch (Throwable t) {
            System.out.println("FAILED: " + t);
            failed++;
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) {
            throw new IllegalStateException(msg);
        }
    }

    private static String pad(String s, int w) {
        StringBuilder sb = new StringBuilder(s);
        while (sb.length() < w) {
            sb.append(' ');
        }
        return sb.toString();
    }
}
