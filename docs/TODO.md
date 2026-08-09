# Known gaps, with what is already understood about each

Things deliberately left, so the next person starts from the diagnosis rather
than from the symptom.

## The `java` launcher eats `=`

`java -Dfoo=bar ...` arrives at the VM as two arguments, `-Dfoo` and `bar`, so
the first non-option token becomes the main class:

```
java -Djava.security.debug=provider -cp x.jar Main
    -> ClassNotFoundException: provider
```

**Not JamVM.** `init.c` parses `-D` correctly -- takes everything after `-D`,
finds the `=`, splits. The damage is upstream, in the launcher, which is an
AmigaDOS script:

```
.KEY args/F
.BRA {
.KET }
SetEnv LD_LIBRARY_PATH "PROGDIR:Sobjs"
SetEnv JAVA_HOME "JAVA:"
JAVA:jamvm-openjdk {args}
```

ReadArgs treats `=` as an argument separator, so it never survives `args/F`.

**Workaround that works today:** quote it.

```
java "-Djava.security.debug=provider" -cp x.jar Main
```

This matters more than a debug flag. Everything tuneable is a system property:
`jdk.tls.client.cipherSuites` (worth ~4.8x on TLS throughput, since the CBC
suites skip GHASH), `jdk.tls.client.protocols`, `sun.nio.fs.chdirAllowed` --
which BUILDING.md tells people to pass, and which has never actually arrived.

**The fix:** a real `java` in C that hands argv to the VM untouched. Two things
already checked, so it should be small:

- `JAVA_HOME` is read by nothing in the VM. `java.home` comes from
  `getJavaHome()`. The line is decorative.
- Every shipped `.so` carries `-Wl,-rpath=JAVA:Sobjs`, an absolute path, so
  `LD_LIBRARY_PATH` is only doing work for a runtime installed somewhere other
  than `JAVA:`. If that case does not need supporting, `java` could simply be a
  copy of `jamvm-openjdk` and the problem disappears with the script.

## `file:/RANDOM:` cannot be opened through Java

`securerandom.source=file:/RANDOM:` fails with

```
java.io.IOException: Failed to open file:/RANDOM:
```

RANDOM: is a real AmigaOS device and open/fopen read it without complaint --
what fails is Java's `file:` URL layer. Worked around by seeding from clib4's
`getentropy()` instead (`SeedGenerator.nativeSeed`), which was worth 25-30
seconds off every JVM start, but the URL path is still broken and other things
will trip over it.

## AES: done, and what it cost to find

Native counter mode landed and GCM went from 506 to 6311 KB/s, a factor of
twelve, with GCTR reporting declined=0 and a byte count matching GHASH's
exactly.  CBC is untouched at 437 -- it goes through AESCrypt.encryptBlock, so
the gap between the two modes is now C against bytecode rather than anything
about the modes.

The lesson worth keeping is not about crypto.  Three builds reported "native"
from reflection while the Java loop did all the work, because a native method
binds only to libraries owned by its own class loader (ClassLoader.findNative,
no fallback to the system list) and GCTR comes from the extension loader.  The
UnsatisfiedLinkError was caught by a robustness handler and thrown away.
Asking "is this method native" is not asking "does this native run"; only the
call counters could tell them apart.

Still interpreted, if CBC suites ever matter: CipherBlockChaining.implEncrypt
and implDecrypt, same shape, same treatment.

## The old AES entry, kept for the reasoning

`AESCrypt.encryptBlock` is still interpreted, and with GHASH now native it is
what the remaining throughput is made of: all four bulk ciphers land in one
narrow band (436-631 KB/s), which is the signature of a shared cost.

The GHASH trick does not transfer. `AESCrypt extends SymmetricCipher implements
AESConstants`, both package-private in the extension loader, so a copy on the
boot class path would extend a *different* `SymmetricCipher` from the one
`CipherBlockChaining` and `GCTR` hold -- ClassCastException. Boot-patching
`SymmetricCipher` too pulls in the whole provider.

That leaves injecting the class into `sunjce_provider.jar`, which is signed,
and therefore also disabling JCE's provider verification in
`javax.crypto.JceSecurity`. Feasible -- `javax.crypto` is boot-loaded, so no
runtime-package trouble -- but it disables a security check and should be a
deliberate, isolated, reversible commit rather than something buried in a build
script.

## Startup is ~11 seconds before the first byte

Down from 25-30 with the entropy fix, and no longer one cause. What is left is
provider registration and 118 CA certificates parsed out of `cacerts`, all
interpreted. Trimming the trust store would help and is the user's call, not
the port's.

## ~400 provider lookups during a transfer

`Cipher.AES/GCM/NoPadding decryption algorithm from: SunJCE` appears roughly
three times per TLS record in a `java.security.debug=provider` trace. Whether
it costs anything measurable has not been established -- worth a look only
after AES, which is certainly dominant.
