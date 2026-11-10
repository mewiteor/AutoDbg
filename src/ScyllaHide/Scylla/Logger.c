#include "Logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static scl_log_write_cb g_write_cb = NULL;
static void *g_write_user = NULL;
static scl_log_cb_a g_cb_a[SCL_LOG_MAX_SEVERITY] = { 0 };
static scl_log_cb_w g_cb_w[SCL_LOG_MAX_SEVERITY] = { 0 };

void scl_log_set_write_cb(scl_log_write_cb cb, void *user)
{
    g_write_cb = cb;
    g_write_user = user;
}

void scl_log_set_cb(scl_log_severity level, scl_log_cb_a cb)
{
    g_cb_a[level] = cb;
}

void scl_log_set_cb_w(scl_log_severity level, scl_log_cb_w cb)
{
    g_cb_w[level] = cb;
}

static void scl_log_write_line(const char *prefix, const char *msg)
{
    char ts[32];
    char *line;
    size_t len;
    time_t now_t;

    time(&now_t);
    struct tm *ltm= localtime(&now_t);
    strftime(ts, sizeof(ts), "%Y.%m.%d %H:%M:%S", ltm);

    len = strlen(ts) + strlen(prefix) + strlen(msg) + 8;
    line = (char *)malloc(len);
    if (line == NULL)
        return;

    _snprintf(line, len, "%s %s: %s", ts, prefix, msg);
    g_write_cb(line, g_write_user);
    free(line);
}

static void scl_log_generic(const char *prefix, scl_log_severity level, const wchar_t *fmt, va_list ap)
{
    va_list vap;
    int wlen;
    wchar_t *wmsg;
    char *amsg;
    int alen;

    // Format the wide message (mirrors the old vfmtw semantics).
    va_copy(vap, ap);
    wlen = _vsnwprintf(NULL, 0, fmt, vap);
    va_end(vap);
    if (wlen < 0)
        wlen = 0;

    wmsg = (wchar_t *)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (wmsg == NULL)
        return;

    va_copy(vap, ap);
    _vsnwprintf(wmsg, (size_t)wlen + 1, fmt, vap);
    va_end(vap);
    wmsg[wlen] = L'\0';

    if (g_cb_w[level])
        g_cb_w[level](wmsg);

    if (g_cb_a[level] || g_write_cb)
    {
        alen = WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, NULL, 0, NULL, NULL);
        amsg = (char *)malloc(alen > 0 ? (size_t)alen : 1);
        if (amsg != NULL)
        {
            WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, amsg, alen, NULL, NULL);

            if (g_cb_a[level])
                g_cb_a[level](amsg);

            if (g_write_cb)
                scl_log_write_line(prefix, amsg);

            free(amsg);
        }
    }

    free(wmsg);
}

void scl_log_debug(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    scl_log_generic("DEBUG", SCL_LOG_DEBUG, fmt, ap);
    va_end(ap);
}

void scl_log_info(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    scl_log_generic("INFO", SCL_LOG_INFO, fmt, ap);
    va_end(ap);
}

void scl_log_error(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    scl_log_generic("ERROR", SCL_LOG_ERROR, fmt, ap);
    va_end(ap);
}
