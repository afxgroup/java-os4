/*
 * amiga_cmdline.h -- build an AmigaDOS command line from an argv array.
 *
 * Shared by the `java` launcher and by java.lang.Runtime.exec, which both hand
 * a command to DOS as a STRING and therefore both have to quote it.  One copy,
 * because the rules are not obvious and two copies drift:
 *
 *   - whitespace separates arguments, so an argument containing any must be
 *     quoted, and 0xA0 (non-breaking space) counts as whitespace here;
 *   - the escape character is '*', NOT backslash: a literal quote is *" and a
 *     literal asterisk is **;
 *   - a newline inside an argument becomes *N, since a raw one would end the
 *     command;
 *   - an empty argument must still be quoted, or it disappears.
 *
 * Getting any of these wrong does not fail loudly.  It turns one argument into
 * two, or swallows the one after it, and the program that receives them reports
 * something unrelated -- which is exactly how the AmigaDOS script this replaced
 * managed to eat '=' out of every -Dfoo=bar for years.
 *
 * Header-only and static: two very different binaries use it (a statically
 * linked launcher and a JNI shared object) and neither wants a library for
 * forty lines.
 *
 * GPLv2 (java-os4 project).
 */
#ifndef AMIGA_CMDLINE_H
#define AMIGA_CMDLINE_H

#include <stdlib.h>
#include <string.h>

static int amiga_arg_needs_quoting(const char *s) {
    const unsigned char *p = (const unsigned char *)s;

    if(*p == '\0') {
        return 1;               /* an empty argument must survive as one */
    }
    for(; *p != '\0'; p++) {
        if(*p == ' ' || *p == '\t' || *p == '\n' || *p == 0xA0 || *p == '"') {
            return 1;
        }
    }
    return 0;
}

/*
 * Append one argument, quoted if it needs to be.  Returns how many characters
 * it would write, so a caller can size the buffer by running it once with a
 * NULL destination -- the same measure-then-write shape as snprintf, and for
 * the same reason: guessing a bound for something that can double in length is
 * how buffers get overrun.
 */
static size_t amiga_quote_arg(char *dst, const char *s) {
    size_t n = 0;

#define AMIGA_PUT(c) do { if(dst != NULL) dst[n] = (c); n++; } while(0)

    if(!amiga_arg_needs_quoting(s)) {
        for(; *s != '\0'; s++) {
            AMIGA_PUT(*s);
        }
        return n;
    }

    AMIGA_PUT('"');
    for(; *s != '\0'; s++) {
        switch(*s) {
            case '"':
            case '*':
                AMIGA_PUT('*');
                AMIGA_PUT(*s);
                break;
            case '\n':
                AMIGA_PUT('*');
                AMIGA_PUT('N');
                break;
            default:
                AMIGA_PUT(*s);
                break;
        }
    }
    AMIGA_PUT('"');
    return n;

#undef AMIGA_PUT
}

/*
 * "program arg1 arg2 ..." with every element quoted as needed.  argv is the
 * arguments ONLY -- the program is passed separately, because a caller that
 * has it in argv[0] would otherwise repeat it.  NULL if out of memory.
 */
static char *amiga_build_command(const char *program,
                                 const char *const *argv, int argc) {
    size_t len = amiga_quote_arg(NULL, program);
    char *cmd;
    size_t n;
    int i;

    for(i = 0; i < argc; i++) {
        len += 1 + amiga_quote_arg(NULL, argv[i]);      /* separating space */
    }

    cmd = (char *)malloc(len + 1);
    if(cmd == NULL) {
        return NULL;
    }

    n = amiga_quote_arg(cmd, program);
    for(i = 0; i < argc; i++) {
        cmd[n++] = ' ';
        n += amiga_quote_arg(cmd + n, argv[i]);
    }
    cmd[n] = '\0';

    return cmd;
}

#endif /* AMIGA_CMDLINE_H */
