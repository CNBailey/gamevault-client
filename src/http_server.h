/* Tiny threaded HTTP/1.1 server (one request per connection). */
#pragma once

#include <stddef.h>
#include "util.h"

typedef struct {
  char method[8];
  char path[1024];   /* decoded path, without the query */
  char query[1024];  /* raw query string, without '?' */
  char *body;        /* NUL-terminated (may be empty) */
  size_t body_len;
} http_req_t;

typedef struct {
  int status;
  const char *content_type;
  const char *extra_headers; /* "Name: value\r\n" lines, or NULL */
  const void *body;          /* static or owned (see owned) */
  size_t body_len;
  char *owned;               /* freed after sending */
} http_res_t;

typedef void (*http_handler_t)(const http_req_t *req, http_res_t *res);

/* Starts listening on port and serves forever on background threads. 0 on success. */
int http_server_start(int port, http_handler_t handler);

/* Helpers for handlers. */
void res_json(http_res_t *res, int status, sbuf_t *json);   /* takes ownership of json */
void res_error(http_res_t *res, int status, const char *message);
/* Reads a query parameter (URL-decoded) into out; returns false when absent. */
bool query_param(const char *query, const char *name, char *out, size_t outlen);
void url_decode(const char *in, char *out, size_t outlen);
