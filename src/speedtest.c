/*
 * Speed test: reads a real game file from GameVault with 1, 2, 4 and 6
 * connections (nothing is written to the drive) and remembers the fewest
 * connections that reach the top speed, for the Automatic setting.
 */
#include "speedtest.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "downloads.h"
#include "http_client.h"
#include "json.h"
#include "settings.h"

#define STEP_MS 4000
#define RAMP_MS 800

typedef struct {
  const char *url;
  int64_t from;
  volatile int *stop;
  int64_t *bytes;
} reader_t;

static int count(void *ctx, const char *data, size_t len) {
  (void)data;
  reader_t *r = ctx;
  __atomic_add_fetch(r->bytes, (int64_t)len, __ATOMIC_RELAXED);
  return *r->stop ? 1 : 0;
}

static void *reader(void *arg) {
  reader_t *r = arg;
  char err[200];
  while (!*r->stop) {
    if (http_get(r->url, r->from, -1, count, r, NULL, r->stop, err, sizeof err) == -1) usleep(200000);
  }
  return NULL;
}

static pthread_mutex_t running = PTHREAD_MUTEX_INITIALIZER;

int speedtest_run(int64_t game_id, sbuf_t *out, char *err, size_t errlen) {
  settings_t s = settings_get();
  if (!s.server[0]) { snprintf(err, errlen, "Set the GameVault address first"); return -1; }
  if (downloads_busy()) { snprintf(err, errlen, "Pause the current download first, so it doesn't skew the test"); return -1; }
  if (pthread_mutex_trylock(&running)) { snprintf(err, errlen, "A speed test is already running"); return -1; }

  /* The largest file of the game makes a long enough read. */
  char url[1400];
  snprintf(url, sizeof url, "%s/api/v1/games/%lld/files", s.server, (long long)game_id);
  sbuf_t body;
  sbuf_init(&body);
  int status = 0;
  int rc = http_get_text(url, &body, &status, err, errlen);
  jval_t *files = rc ? NULL : json_parse(body.data);
  sbuf_free(&body);
  jval_t *list = json_get(files, "files");
  const char *path = NULL;
  int64_t size = 0;
  for (jval_t *f = list ? list->child : NULL; f; f = f->next) {
    int64_t n = (int64_t)json_num(f, "size", 0);
    if (n > size) { size = n; path = json_str(f, "path", NULL); }
  }
  if (!path || size < (256LL << 20)) {
    json_free(files);
    pthread_mutex_unlock(&running);
    if (!rc) snprintf(err, errlen, "Pick a game of at least 256 MB for the speed test");
    return -1;
  }
  sbuf_t u;
  sbuf_init(&u);
  sbuf_printf(&u, "%s/api/v1/games/%lld/files/", s.server, (long long)game_id);
  static const char *hex = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~/", *p)) sbuf_append(&u, p, 1);
    else { char e[3] = { '%', hex[*p >> 4], hex[*p & 15] }; sbuf_append(&u, e, 3); }
  }
  snprintf(url, sizeof url, "%s", u.data);
  sbuf_free(&u);
  char name[256];
  snprintf(name, sizeof name, "%s", path);
  json_free(files);

  static const int counts[] = { 1, 2, 4, 6 };
  double mbps[4] = { 0 };
  for (int k = 0; k < 4; k++) {
    int n = counts[k];
    volatile int stop = 0;
    int64_t bytes = 0;
    reader_t readers[6];
    pthread_t threads[6];
    int started = 0;
    for (int i = 0; i < n; i++) {
      /* Spread the readers over the file, like a real download. */
      readers[i] = (reader_t){ url, (size / n) * i / (1 << 20) * (1 << 20), &stop, &bytes };
      if (pthread_create(&threads[started], NULL, reader, &readers[i]) == 0) started++;
    }
    usleep(RAMP_MS * 1000);
    int64_t b0 = __atomic_load_n(&bytes, __ATOMIC_RELAXED), t0 = now_ms();
    usleep((STEP_MS - RAMP_MS) * 1000);
    int64_t b1 = __atomic_load_n(&bytes, __ATOMIC_RELAXED), t1 = now_ms();
    stop = 1;
    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    mbps[k] = (double)(b1 - b0) / (1024.0 * 1024.0) / ((double)(t1 - t0) / 1000.0);
    log_msg("speed test: %d connection(s) %.1f MB/s", n, mbps[k]);
  }
  pthread_mutex_unlock(&running);

  double top = 0;
  for (int k = 0; k < 4; k++) if (mbps[k] > top) top = mbps[k];
  int best = 0;
  for (int k = 0; k < 4 && !best; k++) if (mbps[k] >= top * 0.95) best = counts[k];
  if (top <= 0) { snprintf(err, errlen, "No data came through from GameVault"); return -1; }

  settings_t now = settings_get();
  now.measured_connections = best;
  now.measured_mbps = (int)(top + 0.5);
  settings_set(&now);

  sbuf_puts(out, "{");
  jw_kv_str(out, "file", name, false);
  jw_kv_int(out, "best", best, true);
  sbuf_puts(out, ",\"results\":[");
  for (int k = 0; k < 4; k++) {
    if (k) sbuf_puts(out, ",");
    sbuf_printf(out, "{\"connections\":%d,\"mbps\":%.1f}", counts[k], mbps[k]);
  }
  sbuf_puts(out, "]}");
  return 0;
}
