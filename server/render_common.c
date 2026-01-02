/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_common.h"
#ifndef STANDALONE_SERVER
#include "virgl_util.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

void
render_log_init(void)
{
#ifdef STANDALONE_SERVER
   openlog(NULL, LOG_NDELAY | LOG_PERROR | LOG_PID, LOG_USER);
#endif
}

void
render_log(const char *fmt, ...)
{
   va_list va;

   va_start(va, fmt);
#ifdef STANDALONE_SERVER
   vsyslog(LOG_DEBUG, fmt, va);
#else
   virgl_prefixed_logv("server", VIRGL_LOG_LEVEL_INFO, fmt, va);
#endif
   va_end(va);
}
