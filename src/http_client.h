/* Minimal HTTP/1.1 GET client (plain http://), with Range support. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "util.h"

typedef struct {
  int status;
  int64_t content_length; /* -1 when unknown */
  int64_t total_size;     /* from Content-Range "bytes a-b/TOTAL", else content_length */
} http_info_t;

/* Return non-zero to stop the transfer. */
typedef int (*http_body_cb)(void *ctx, const char *data, size_t len);

/*
 * GET url. When from >= 0 sends "Range: bytes=from-to" (to < 0 means to the end).
 * `stop` is polled while reading; set it to non-zero to abort.
 * Returns 0 when the whole body was received and the status was 2xx.
 */
int http_get(const char *url, int64_t from, int64_t to, http_body_cb cb, void *ctx,
             http_info_t *info, volatile int *stop, char *err, size_t errlen);

/* GET into memory (bodies up to 8 MB). */
int http_get_text(const char *url, sbuf_t *out, int *status, char *err, size_t errlen);
