#include <pthread.h>
#include <time.h>
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void sbuf_init(sbuf_t *b) { b->data = NULL; b->len = 0; b->cap = 0; }

void sbuf_free(sbuf_t *b) { free(b->data); sbuf_init(b); }

static void sbuf_reserve(sbuf_t *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < b->len + extra + 1) cap *= 2;
  char *p = realloc(b->data, cap);
  if (!p) abort();
  b->data = p;
  b->cap = cap;
}

void sbuf_append(sbuf_t *b, const void *data, size_t len) {
  sbuf_reserve(b, len);
  memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = 0;
}

void sbuf_puts(sbuf_t *b, const char *s) { sbuf_append(b, s, strlen(s)); }

void sbuf_printf(sbuf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) { va_end(ap2); return; }
  sbuf_reserve(b, (size_t)n);
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  b->len += (size_t)n;
}

char *sbuf_detach(sbuf_t *b) {
  if (!b->data) sbuf_append(b, "", 0);
  char *s = b->data;
  sbuf_init(b);
  return s;
}

bool url_parse(const char *url, url_t *out) {
  memset(out, 0, sizeof *out);
  if (strncmp(url, "http://", 7) != 0) return false;
  const char *p = url + 7;
  const char *slash = strchr(p, '/');
  size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
  if (hostlen == 0 || hostlen >= sizeof out->host) return false;
  memcpy(out->host, p, hostlen);
  out->port = 80;
  char *colon = strrchr(out->host, ':');
  if (colon && !strchr(out->host, ']')) {
    *colon = 0;
    out->port = atoi(colon + 1);
    if (out->port <= 0 || out->port > 65535) return false;
  }
  snprintf(out->path, sizeof out->path, "%s", slash ? slash : "/");
  return true;
}

int mkdir_p(const char *path) {
  char tmp[1024];
  snprintf(tmp, sizeof tmp, "%s", path);
  size_t len = strlen(tmp);
  if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
  return 0;
}

bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t file_size(const char *path) {
  struct stat st;
  if (stat(path, &st)) return -1;
  return (int64_t)st.st_size;
}

char *read_file(const char *path, size_t *len_out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  sbuf_t b;
  sbuf_init(&b);
  char buf[8192];
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) sbuf_append(&b, buf, (size_t)n);
  close(fd);
  if (n < 0) { sbuf_free(&b); return NULL; }
  if (len_out) *len_out = b.len;
  return sbuf_detach(&b);
}

int write_file_atomic(const char *path, const char *data, size_t len) {
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n <= 0) { close(fd); unlink(tmp); return -1; }
    off += (size_t)n;
  }
  fsync(fd);
  close(fd);
  return rename(tmp, path);
}

int rm_rf(const char *path) {
  struct stat st;
  if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode)) return unlink(path);
  DIR *d = opendir(path);
  if (!d) return -1;
  struct dirent *e;
  int rc = 0;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[1024];
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    if (rm_rf(child)) rc = -1;
  }
  closedir(d);
  if (rmdir(path)) rc = -1;
  return rc;
}

int64_t now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void iso_time(char out[32], int64_t ms) {
  time_t t = (time_t)(ms / 1000);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* The last log lines, shown in Settings › Diagnostics. */
#define LOG_LINES 200
static char log_ring[LOG_LINES][300];
static int log_next, log_count;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_msg(const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  fprintf(stderr, "[gamevault] %s\n", line);
  char stamp[32];
  time_t t = time(NULL);
  strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&t));
  pthread_mutex_lock(&log_lock);
  snprintf(log_ring[log_next], sizeof log_ring[0], "%s %s", stamp, line);
  log_next = (log_next + 1) % LOG_LINES;
  if (log_count < LOG_LINES) log_count++;
  pthread_mutex_unlock(&log_lock);
}

void log_lines(void (*each)(const char *line, void *ctx), void *ctx) {
  pthread_mutex_lock(&log_lock);
  for (int i = 0; i < log_count; i++) each(log_ring[(log_next - log_count + i + LOG_LINES) % LOG_LINES], ctx);
  pthread_mutex_unlock(&log_lock);
}

bool safe_name(const char *name) {
  if (!name || !*name || name[0] == '.') return false;
  for (const char *p = name; *p; p++) {
    if (*p == '/' || *p == '\\') return false;
  }
  return true;
}

bool path_inside(const char *root, const char *path) {
  size_t rl = strlen(root);
  if (strncmp(root, path, rl) != 0) return false;
  if (path[rl] != '/' ) return false;
  if (strstr(path, "/../") || strstr(path, "/./")) return false;
  size_t pl = strlen(path);
  if (pl >= 3 && !strcmp(path + pl - 3, "/..")) return false;
  return true;
}
