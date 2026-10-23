// Logging to logcat and to a file. Some phones (vivo, for one) drop logcat
// output from the game process, so each library also writes DBG_FILE, which
// is truncated on every launch. Define DBG_FILE before including this.
#pragma once
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>

static FILE* dbg_fp;
static int dbg_attempts;

__attribute__((format(printf, 3, 4))) static void dbg_log(int prio, const char* tag, const char* fmt, ...) {
    char buf[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_print(prio, tag, "%s", buf);
    // The directory only exists once Java has asked for it, so keep retrying for a while.
    if (!dbg_fp && dbg_attempts < 200) {
        dbg_attempts++;
        dbg_fp = fopen(DBG_FILE, "w");
    }
    if (dbg_fp) {
        fprintf(dbg_fp, "%c %s\n", prio >= ANDROID_LOG_WARN ? 'W' : 'I', buf);
        fflush(dbg_fp);
    }
}
