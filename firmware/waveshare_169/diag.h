// Log lines for the web diagnostics (GET /log, web.cpp). Without APP_WEB_UI the
// macros compile to nothing.
//
// Each task logs into its own lock-free ring, so use the macro of the task the
// code runs on: DLOG_USB on EspUsbHost's client task, DLOG_LOADER on the loader
// task, DLOG_LOOP on the Arduino loop. A log call never blocks or allocates; a
// full ring drops the line and counts it (GET /status shows the counts).
#pragma once

#include "apps.h"

#if APP_WEB_UI
#include <string>

void diag_log(char source, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define DLOG_USB(...) diag_log('U', __VA_ARGS__)
#define DLOG_LOADER(...) diag_log('L', __VA_ARGS__)
#define DLOG_LOOP(...) diag_log('M', __VA_ARGS__)
// Loader task: GET /store's body, rebuilt whenever the store may have changed.
void diag_publish_store(std::string &&json);
#else
#define DLOG_USB(...) do {} while (0)
#define DLOG_LOADER(...) do {} while (0)
#define DLOG_LOOP(...) do {} while (0)
#endif
