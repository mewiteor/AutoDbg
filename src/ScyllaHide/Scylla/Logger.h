#pragma once

#include <stdarg.h>
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum scl_log_severity
{
    SCL_LOG_DEBUG = 0,
    SCL_LOG_INFO,
    SCL_LOG_ERROR,
    SCL_LOG_MAX_SEVERITY
} scl_log_severity;

// Receives a fully formatted log line (timestamp + severity prefix + message),
// without a trailing newline. `user` is the pointer passed to scl_log_set_write_cb.
typedef void (*scl_log_write_cb)(const char *line, void *user);

// Raw message callbacks, one per severity.
typedef void (*scl_log_cb_a)(const char *msg);
typedef void (*scl_log_cb_w)(const wchar_t *msg);

void scl_log_set_write_cb(scl_log_write_cb cb, void *user);
void scl_log_set_cb(scl_log_severity level, scl_log_cb_a cb);
void scl_log_set_cb_w(scl_log_severity level, scl_log_cb_w cb);

void scl_log_debug(const wchar_t *fmt, ...);
void scl_log_info(const wchar_t *fmt, ...);
void scl_log_error(const wchar_t *fmt, ...);

#ifdef __cplusplus
}
#endif
