/* GameVault PS5 client - small shared helpers. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Growable byte/string buffer. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sbuf_t;

void sbuf_init(sbuf_t *b);
void sbuf_free(sbuf_t *b);
void sbuf_append(sbuf_t *b, const void *data, size_t len);
void sbuf_puts(sbuf_t *b, const char *s);
void sbuf_printf(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *sbuf_detach(sbuf_t *b); /* returns NUL-terminated string, caller frees */

/* "http://host:port/path?x" -> parts. Returns false when not plain http://. */
typedef struct {
  char host[256];
  int port;
  char path[2048];
} url_t;
bool url_parse(const char *url, url_t *out);

int mkdir_p(const char *path);
bool file_exists(const char *path);
bool dir_exists(const char *path);
int64_t file_size(const char *path);
char *read_file(const char *path, size_t *len_out); /* NUL-terminated, caller frees */
int write_file_atomic(const char *path, const char *data, size_t len);
int rm_rf(const char *path);

int64_t now_ms(void);
void iso_time(char out[32], int64_t ms);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Safe path component check (no "/", "..", NUL, leading dot). */
bool safe_name(const char *name);
/* True when `path` is inside `root` (after resolving ".." textually). */
bool path_inside(const char *root, const char *path);
