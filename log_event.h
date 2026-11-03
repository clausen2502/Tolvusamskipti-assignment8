/* ====================================================================
 * log_event.h -- server.log writer for Assignment 8.
 *
 * Drop it next to your own source files and include it:
 *
 *     #include "log_event.h"
 *
 * It writes the fixed log format required by the "Server Log" section
 * of the assignment, so you do not have to get the timestamp, the
 * field layout or the escaping right yourself.
 *
 *     log_event("START", NULL, 0, GROUP_ID, "listening on 4001");
 *     log_event("ACCEPT", ip, port, NULL, "incoming connection");
 *     log_event_bytes("RECV", ip, port, peer_id, cmd, cmd_len);
 *     log_note("no route to %s, holding %d messages", id, n);
 *
 * Pass NULL for anything you do not know, and the field becomes "-".
 *
 * Three functions, because the detail is three different kinds of
 * thing:
 *
 *   log_event()        the detail is a C string -- almost everything
 *   log_event_bytes()  the detail is a buffer and a length. Use this
 *                      for the commands you RECV and SEND: message
 *                      content is arbitrary data and may contain NUL
 *                      bytes, which a C string would cut short.
 *   log_note()         not a protocol event at all. printf-style, and
 *                      it writes a line that cannot be mistaken for
 *                      one of the seven events below.
 *
 * Plain ISO C, so it compiles as C or C++ with no extra flags and no
 * libraries beyond the standard one. Including it from several .c
 * files that are compiled separately and linked together is fine --
 * everything here is static, so there is nothing to collide.
 *
 * ---------------------------------------------------------------------
 * THREADS
 *
 * You do not have to read this unless your server uses threads. The
 * short version: it is safe, as long as something has been logged
 * before you start any thread -- and your START line already does
 * that.
 *
 * Why it works. Each call builds its line in a local buffer and
 * writes it with a single fwrite, so two threads cannot interleave
 * halves of a line. Nothing here keeps a scratch buffer in shared
 * state, and the timestamp is computed without gmtime(), which would
 * have been a shared one.
 *
 * The one gap. The log file is opened on first use:
 *
 *     static FILE *fp = NULL;
 *     if (fp == NULL) { fp = fopen(...); }
 *
 * Two threads reaching that check at the same moment could both open
 * the file, and one of the two handles would then be leaked. After the
 * first successful call there is nothing left to race on. Logging your
 * START line before the first thread starts avoids it entirely, which
 * is what the assignment has you do anyway.
 *
 * Two things that are true but harmless: the one-off warnings on
 * stderr can appear twice, and lines logged at very nearly the same
 * moment may land in the file in the other order. Every line carries
 * its own timestamp, so nothing that reads the log depends on the
 * order they happen to be written in.
 *
 * Do NOT call these from a signal handler. fopen() and fwrite() are
 * not async-signal-safe, and that is a property of stdio rather than
 * something this file could fix. Set a flag in the handler and log
 * from your main loop.
 *
 * ---------------------------------------------------------------------
 *
 * You may change this file, but then the format is your problem again:
 * it is the FORMAT that is fixed, not this implementation. If you do
 * change it, say so in your README.md.
 *
 * ---------------------------------------------------------------------
 * THE FORMAT
 *
 * A line recording a protocol event has five fields, separated by
 * single spaces:
 *
 *     <TIMESTAMP> <EVENT> <IP:PORT> <GROUP_ID> <detail>
 *
 * TIMESTAMP  ISO 8601, in UTC, e.g. 2026-10-26T14:03:27Z. Always UTC,
 *            even if your machine is set to local time -- that is what
 *            makes logs from different machines comparable.
 *
 * EVENT      one of seven keywords:
 *              START   your server started up
 *              ACCEPT  you accepted an incoming connection
 *              CONN    you established an outgoing connection
 *              RECV    you received a complete, well-formed command
 *              SEND    you sent a command
 *              DROP    you received something you did not accept as a
 *                      command (malformed or over-long frame, unknown
 *                      command, ...)
 *              CLOSE   a connection was closed or lost
 *
 * IP:PORT    the other end of the connection the event happened on.
 *            A single "-" when the event is not about a connection,
 *            e.g. START.
 *
 * GROUP_ID   the group id of the server at the other end. A single
 *            "-" when you do not know it yet -- before that server has
 *            sent its HELO, or for a connection from your own client.
 *            For START, your OWN group id.
 *
 * detail     for RECV and SEND, the command exactly as it went over
 *            the wire, WITHOUT the <SOH><length><STX> ... <ETX>
 *            framing bytes. Anything you find useful for the other
 *            events. Runs to the end of the line, so it may contain
 *            spaces and commas.
 *
 * Any line whose second field is not one of the seven keywords is an
 * additional, free-form line. Those are allowed and encouraged -- but
 * they must start with a timestamp too, so that every line in the file
 * can be placed in time. log_note() writes them with "note" as the
 * second field, which keeps them clear of the keywords above.
 *
 * ---------------------------------------------------------------------
 * WHAT IT LOOKS LIKE
 *
 * The first few lines of a session, with a peer connecting to us:
 *
 *   2026-10-26T14:03:11Z START - G_banana listening on port 4001
 *   2026-10-26T14:03:27Z ACCEPT 130.208.246.98:51314 - incoming
 *   2026-10-26T14:03:27Z RECV 130.208.246.98:51314 - HELO,G_pelican
 *   2026-10-26T14:03:27Z SEND 130.208.246.98:51314 G_pelican SERVERS,...
 *   2026-10-26T14:03:27Z SEND 130.208.246.98:51314 G_pelican HELO,G_banana
 *   2026-10-26T14:03:28Z RECV 130.208.246.98:51314 G_pelican SENDMSG,...
 *   2026-10-26T14:03:28Z note no route to G_tapir, holding 3 for it
 *   2026-10-26T14:55:40Z CLOSE 130.208.246.98:51314 G_pelican peer gone
 *
 * Note the third line: a peer's HELO is what tells you its group id,
 * so on the line recording that very HELO you do not know it yet and
 * the field is "-". From the next line on it is G_pelican.
 *
 * ---------------------------------------------------------------------
 * ESCAPING
 *
 * Message content is arbitrary data. A line feed in it would split one
 * log line into two, and a NUL byte would cut it short, so this file
 * escapes them: backslash, CR, LF and tab become \\, \r, \n and \t,
 * and any other control byte becomes \xHH. Bytes >= 0x80 are left
 * alone, so UTF-8 content stays legible.
 *
 * In the four fields that have to stay single tokens, a space is
 * escaped as \x20 as well. That matters for the group id: a HELO is an
 * unverified claim, so a peer can send you a "name" with a space in
 * it, and writing it raw would turn one event into six fields.
 * ==================================================================== */

#ifndef LOG_EVENT_H
#define LOG_EVENT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The log file. Fixed by the assignment; override at build time only if
 * you have a reason to (-DLOG_EVENT_PATH="\"other.log\""). */
#ifndef LOG_EVENT_PATH
#define LOG_EVENT_PATH "server.log"
#endif

#if defined(__GNUC__)
#define LOG_EVENT_UNUSED __attribute__((unused))
#define LOG_EVENT_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define LOG_EVENT_UNUSED
#define LOG_EVENT_PRINTF(a, b)
#endif

/* --------------------------------------------------------------------
 * Internals
 * -------------------------------------------------------------------- */

/* The seven event keywords, for the typo check in log_event(). */
static const char *const log__events[] = {
    "START", "ACCEPT", "CONN", "RECV", "SEND", "DROP", "CLOSE"
};

/* Open on first use and keep it open, in APPEND mode: restarting your
 * server must add to server.log, never start it again from scratch.
 *
 * The NULL check is the one place threads can race -- see THREADS at
 * the top of this file. Two threads arriving here together could both
 * fopen() and leak one handle; after the first successful call there
 * is nothing to race on. Logging START before any thread starts
 * avoids it, and is what the assignment has you do anyway.
 *
 * Deliberately NOT guarded by a mutex: that would need <pthread.h>
 * and -pthread, and this header stays ISO C so that it compiles for
 * every group whatever their build flags are. See README.md,
 * "Implementation notes for log_event.h". */
LOG_EVENT_UNUSED static FILE *log__file(void)
{
    static FILE *fp = NULL;
    static int complained = 0;

    if (fp == NULL) {
        fp = fopen(LOG_EVENT_PATH, "ab");
        if (fp == NULL && !complained) {
            complained = 1;
            fprintf(stderr, "log_event: cannot open %s for appending\n",
                    LOG_EVENT_PATH);
        }
    }
    return fp;
}

/* Days from 1970-01-01 to y-m-d. Hinnant's civil calendar algorithm. */
LOG_EVENT_UNUSED static void log__civil_from_days(long z, long *y, unsigned *m,
                                                  unsigned *d)
{
    long era, doe, yoe, doy, mp, year;
    unsigned month, day;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                  /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    year = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);             /* [0, 365]  */
    mp = (5 * doy + 2) / 153;                                 /* [0, 11]   */
    day = (unsigned)(doy - (153 * mp + 2) / 5 + 1);           /* [1, 31]   */
    month = (unsigned)(mp < 10 ? mp + 3 : mp - 9);            /* [1, 12]   */

    *y = year + (month <= 2);
    *m = month;
    *d = day;
}

/* "2026-10-26T14:03:27Z" in UTC. */
LOG_EVENT_UNUSED static void log__timestamp(char *out, size_t n)
{
    time_t now = time(NULL);
    long days, secs, y;
    unsigned m, d;

    if (now == (time_t)-1) {
        snprintf(out, n, "1970-01-01T00:00:00Z");
        return;
    }

    /* Floor division, so pre-1970 clocks do not produce nonsense. */
    days = (long)(now / 86400);
    secs = (long)(now % 86400);
    if (secs < 0) {
        secs += 86400;
        days -= 1;
    }

    log__civil_from_days(days, &y, &m, &d);
    snprintf(out, n, "%04ld-%02u-%02uT%02ld:%02ld:%02ldZ", y, m, d,
             secs / 3600, (secs / 60) % 60, secs % 60);
}

/* Escape one byte into buf, returning how many bytes were written.
 *
 * Backslash, CR and LF become \\, \r and \n, as the assignment
 * specifies. Other control bytes, and DEL, become \xHH -- a raw one
 * (NUL especially) would make the line unreadable. Space is escaped in
 * the fields that have to stay single tokens, but not in the detail,
 * which runs to the end of the line. Bytes >= 0x80 pass through, so
 * UTF-8 message content stays legible. */
LOG_EVENT_UNUSED static size_t log__escape_byte(unsigned char c, char *buf,
                                                int escape_space)
{
    static const char hex[] = "0123456789abcdef";

    if (c == '\\') { buf[0] = '\\'; buf[1] = '\\'; return 2; }
    if (c == '\r') { buf[0] = '\\'; buf[1] = 'r';  return 2; }
    if (c == '\n') { buf[0] = '\\'; buf[1] = 'n';  return 2; }
    if (c == '\t') { buf[0] = '\\'; buf[1] = 't';  return 2; }
    if (c == ' ' && !escape_space) { buf[0] = ' '; return 1; }

    if (c < 0x20 || c == 0x7F || c == ' ') {
        buf[0] = '\\';
        buf[1] = 'x';
        buf[2] = hex[(c >> 4) & 0xF];
        buf[3] = hex[c & 0xF];
        return 4;
    }

    buf[0] = (char)c;
    return 1;
}

/* --------------------------------------------------------------------
 * Building a line in memory
 *
 * A line is assembled in a buffer and written with ONE fwrite, rather
 * than with a series of fputc/fputs calls straight to the stream.
 *
 * That is not about speed. A long line -- a 5000-byte SENDMSG whose
 * content escapes to several times that -- is larger than the stdio
 * buffer, so writing it piecemeal lets stdio flush half a line and
 * then the rest. With one writer that is invisible. But if you include
 * this header from two .c files, each gets its own FILE* for the same
 * file (the handle above is static), and then the other writer's line
 * can land in the gap, leaving two corrupted lines in the file that is
 * your evidence for most of the assignment.
 *
 * One fwrite of a complete line is not guaranteed atomic by the C
 * standard either, but in practice a single write of less than
 * PIPE_BUF/BUFSIZ to a file opened in append mode does not interleave,
 * and it removes the case that actually bites.
 *
 * The buffer below is a LOCAL in each of the three public functions,
 * not a static, which is also what makes them safe to call from
 * several threads: there is no shared scratch space to corrupt. It
 * costs ~24 KB of stack per call, which is nothing on a main thread
 * and fine on a default 8 MB pthread stack -- but worth knowing if you
 * set a small stack size yourself.
 * -------------------------------------------------------------------- */

/* Room for the longest line we will ever write: the assignment caps a
 * command at 5000 bytes, every byte of which can escape to four
 * characters, plus the timestamp and the other three fields. */
#define LOG__LINE_MAX 24576

typedef struct {
    char  buf[LOG__LINE_MAX];
    size_t len;
    int   truncated;
} log__line;

LOG_EVENT_UNUSED static void log__init(log__line *L)
{
    L->len = 0;
    L->truncated = 0;
}

/* Append one byte; silently stop at the end of the buffer rather than
 * overrunning it. Truncating a pathological line is the right failure:
 * the line stays well formed and the rest of the log is unaffected. */
LOG_EVENT_UNUSED static void log__putc(log__line *L, char c)
{
    if (L->len < sizeof L->buf) {
        L->buf[L->len++] = c;
    } else {
        L->truncated = 1;
    }
}

LOG_EVENT_UNUSED static void log__puts(log__line *L, const char *s)
{
    while (*s != '\0') {
        log__putc(L, *s++);
    }
}

/* Append len bytes of s, escaped. escape_space is set for the fields
 * that must stay single tokens. */
LOG_EVENT_UNUSED static void log__put_escaped(log__line *L, const char *s,
                                              size_t len, int escape_space)
{
    char buf[4];
    size_t i, n, k;

    for (i = 0; i < len; i++) {
        n = log__escape_byte((unsigned char)s[i], buf, escape_space);
        for (k = 0; k < n; k++) {
            log__putc(L, buf[k]);
        }
    }
}

/* A field that is absent is written as a single "-". */
LOG_EVENT_UNUSED static void log__put_field(log__line *L, const char *s)
{
    if (s == NULL || s[0] == '\0') {
        log__putc(L, '-');
    } else {
        log__put_escaped(L, s, strlen(s), 1);
    }
}

/* Terminate the line and write it in one go. */
LOG_EVENT_UNUSED static void log__commit(log__line *L, FILE *fp)
{
    log__putc(L, '\n');

    /* If the line filled the buffer, the newline was dropped with
     * everything else, so put one back: a log line without a line
     * ending would swallow the next line too. */
    if (L->len == 0 || L->buf[L->len - 1] != '\n') {
        L->buf[sizeof L->buf - 1] = '\n';
        L->len = sizeof L->buf;
    }

    fwrite(L->buf, 1, L->len, fp);
    fflush(fp);                  /* a crash must not cost you the evidence */
}

/* --------------------------------------------------------------------
 * The three functions you call
 * -------------------------------------------------------------------- */

/* Log one protocol event whose detail is a buffer of known length:
 *
 *   <TIMESTAMP> <EVENT> <IP:PORT> <GROUP_ID> <detail>
 *
 *   event      one of START, ACCEPT, CONN, RECV, SEND, DROP, CLOSE.
 *   ip, port   the other end of the connection; pass NULL for ip when
 *              the event is not about a connection (e.g. START), and
 *              the IP:PORT field becomes "-".
 *   group_id   the peer's group ID, or NULL if you do not know it yet
 *              (before its HELO, or for your own client). For START,
 *              pass your own GROUP_ID.
 *   detail     for RECV and SEND, the command WITHOUT its <SOH><length>
 *              <STX> ... <ETX> framing bytes; anything useful for the
 *              other events; NULL for nothing.
 *   detail_len how many bytes of detail to log.
 *
 * Use this for the commands you RECV and SEND. Message content is
 * arbitrary data: it may contain NUL bytes and newlines, so it is
 * taken as detail_len bytes and escaped rather than treated as a C
 * string. For everything else log_event() below is shorter.
 */
LOG_EVENT_UNUSED static void log_event_bytes(const char *event, const char *ip,
                                             int port, const char *group_id,
                                             const void *detail,
                                             size_t detail_len)
{
    FILE *fp = log__file();
    char stamp[32];
    char portbuf[16];
    log__line line;
    size_t i;
    int known = 0;

    if (fp == NULL) {
        return;                      /* already complained once on stderr */
    }

    /* A mistyped or lower-case keyword turns a protocol event into a
     * free-form line, silently costing you the points that line was
     * the evidence for. Cheap to catch here. */
    for (i = 0; i < sizeof log__events / sizeof log__events[0]; i++) {
        if (event != NULL && strcmp(event, log__events[i]) == 0) {
            known = 1;
            break;
        }
    }
    if (!known) {
        /* Per translation unit, so a server split across several .c
         * files can warn once each, and two threads hitting this
         * together can warn twice. Neither misleads anybody. */
        static int complained = 0;
        if (!complained) {
            complained = 1;
            fprintf(stderr,
                    "log_event: \"%s\" is not one of the seven event "
                    "keywords; this line will be read as a free-form "
                    "note. Use log_note() for those.\n",
                    event == NULL ? "(null)" : event);
        }
    }

    log__init(&line);

    log__timestamp(stamp, sizeof stamp);
    log__puts(&line, stamp);

    log__putc(&line, ' ');
    log__put_field(&line, event);

    log__putc(&line, ' ');
    if (ip == NULL || ip[0] == '\0') {
        log__putc(&line, '-');
    } else {
        log__put_escaped(&line, ip, strlen(ip), 1);
        snprintf(portbuf, sizeof portbuf, ":%d", port);
        log__puts(&line, portbuf);
    }

    log__putc(&line, ' ');
    log__put_field(&line, group_id);

    log__putc(&line, ' ');
    if (detail == NULL || detail_len == 0) {
        log__putc(&line, '-');
    } else {
        log__put_escaped(&line, (const char *)detail, detail_len, 0);
    }

    log__commit(&line, fp);
}

/* The same, for a detail that is a plain C string -- which is almost
 * everything except the commands you RECV and SEND.
 *
 *     log_event("START", NULL, 0, GROUP_ID, "listening on 4001");
 *     log_event("CLOSE", ip, port, peer_id, "peer disconnected");
 *     log_event("ACCEPT", ip, port, NULL, NULL);   -- detail becomes "-"
 */
LOG_EVENT_UNUSED static void log_event(const char *event, const char *ip,
                                       int port, const char *group_id,
                                       const char *detail)
{
    log_event_bytes(event, ip, port, group_id,
                    detail, detail == NULL ? 0 : strlen(detail));
}

/* Log a free-form line: whatever your server decided and why, what it
 * is holding, when it gave up on something. Takes printf arguments.
 *
 *   <TIMESTAMP> note <your text>
 *
 * The literal second field "note" is what keeps these lines from being
 * mistaken for protocol events, so log anything that is not one of the
 * seven events through here rather than through log_event().
 *
 * Newlines in the text are escaped, so one call is always one line.
 */
LOG_EVENT_PRINTF(1, 2)
LOG_EVENT_UNUSED static void log_note(const char *fmt, ...)
{
    FILE *fp = log__file();
    char stamp[32];
    char text[2048];
    log__line line;
    va_list ap;
    int n;

    if (fp == NULL) {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof text) {
        n = (int)sizeof text - 1;    /* truncated; still one valid line */
    }

    log__init(&line);
    log__timestamp(stamp, sizeof stamp);
    log__puts(&line, stamp);
    log__puts(&line, " note ");
    log__put_escaped(&line, text, (size_t)n, 0);
    log__commit(&line, fp);
}

#endif /* LOG_EVENT_H */
