/*
 * snowcone/include/sc_log.h — Logging utilities.
 *
 * Simple fprintf-based logging to stderr. LOGV messages are only
 * emitted when g_verbose is set (via the -v flag).
 */

#ifndef SC_LOG_H
#define SC_LOG_H

#include <stdarg.h>
#include <stdio.h>

extern int g_verbose;

static inline void sc_logmsg(const char *prefix, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "snowcone: %s ", prefix);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define LOGV(...) do { if (g_verbose) sc_logmsg("[v]", __VA_ARGS__); } while (0)
#define LOGI(...) sc_logmsg("[i]", __VA_ARGS__)
#define LOGE(...) sc_logmsg("[!]", __VA_ARGS__)

#endif /* SC_LOG_H */