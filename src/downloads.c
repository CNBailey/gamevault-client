#include "downloads.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "console.h"
#include "http_client.h"
#include "json.h"
#include "platform.h"
#include "settings.h"

#define CHUNK ((int64_t)64 << 20)
#define WRITE_BUF (8 << 20)   /* write to the drive in large blocks */
#define MAX_JOBS 256
#define MAX_RETRIES 6

typedef struct {
  char path[512];      /* path on the server, relative to the game */
  char url[1400];      /* full URL */
  char category[16];   /* PS4 pkg category: gd, gp, ac ... */
  int64_t size;
  char dest[768];      /* console path of the finished file (PS5) */
  int64_t done;
} dfile_t;

typedef struct {
  char id[24];
  int64_t game_id;
  char title[256], title_id[16], platform[8], version[32], content_id[64];
  char cover[1024], drive[16], state[16], error[256], note[256], replace[512];
  int64_t total, done, created, started, finished;
  double speed;
  dfile_t *files;
  int nfiles;
  volatile int stop;   /* 1 = pause, 2 = cancel */
} job_t;

static job_t *jobs[MAX_JOBS];
static int njobs;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int id_counter;
static volatile int halted;   /* set before a restart: start nothing new */

/* ---- persistence --------------------------------------------------------------- */

static void jobs_path(char *out, size_t n) { snprintf(out, n, "%s/jobs.json", platform_data_dir()); }

static void job_json(const job_t *j, sbuf_t *b, bool with_files) {
  sbuf_puts(b, "{");
  jw_kv_str(b, "id", j->id, false);
  jw_kv_int(b, "gameId", j->game_id, true);
  jw_kv_str(b, "title", j->title, true);
  jw_kv_str(b, "titleId", j->title_id, true);
  jw_kv_str(b, "platform", j->platform, true);
  jw_kv_str(b, "version", j->version, true);
  jw_kv_str(b, "contentId", j->content_id, true);
  jw_kv_str(b, "cover", j->cover, true);
  jw_kv_str(b, "drive", j->drive, true);
  jw_kv_str(b, "state", j->state, true);
  jw_kv_str(b, "error", j->error, true);
  jw_kv_str(b, "note", j->note, true);
  jw_kv_str(b, "replace", j->replace, true);
  jw_kv_int(b, "total", j->total, true);
  jw_kv_int(b, "done", j->done, true);
  jw_kv_int(b, "speed", (int64_t)j->speed, true);
  jw_kv_int(b, "created", j->created, true);
  jw_kv_int(b, "started", j->started, true);
  jw_kv_int(b, "finished", j->finished, true);
  if (with_files) {
    sbuf_puts(b, ",\"files\":[");
    for (int i = 0; i < j->nfiles; i++) {
      const dfile_t *f = &j->files[i];
      if (i) sbuf_puts(b, ",");
      sbuf_puts(b, "{");
      jw_kv_str(b, "path", f->path, false);
      jw_kv_str(b, "url", f->url, true);
      jw_kv_str(b, "category", f->category, true);
      jw_kv_int(b, "size", f->size, true);
      jw_kv_str(b, "dest", f->dest, true);
      jw_kv_int(b, "done", f->done, true);
      sbuf_puts(b, "}");
    }
    sbuf_puts(b, "]");
  } else {
    jw_kv_int(b, "fileCount", j->nfiles, true);
  }
  sbuf_puts(b, "}");
}

/* Caller holds lock. */
static void save_locked(void) {
  sbuf_t b;
  sbuf_init(&b);
  sbuf_puts(&b, "[");
  for (int i = 0; i < njobs; i++) {
    if (i) sbuf_puts(&b, ",\n");
    job_json(jobs[i], &b, true);
  }
  sbuf_puts(&b, "]\n");
  char p[600];
  jobs_path(p, sizeof p);
  mkdir_p(platform_data_dir());
  write_file_atomic(p, b.data, b.len);
  sbuf_free(&b);
}

static void copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

static void load(void) {
  char p[600];
  jobs_path(p, sizeof p);
  char *text = read_file(p, NULL);
  if (!text) return;
  jval_t *arr = json_parse(text);
  free(text);
  if (!arr || arr->type != J_ARR) { json_free(arr); return; }
  for (jval_t *v = arr->child; v && njobs < MAX_JOBS; v = v->next) {
    job_t *j = calloc(1, sizeof *j);
    copy(j->id, sizeof j->id, json_str(v, "id", ""));
    j->game_id = (int64_t)json_num(v, "gameId", 0);
    copy(j->title, sizeof j->title, json_str(v, "title", ""));
    copy(j->title_id, sizeof j->title_id, json_str(v, "titleId", ""));
    copy(j->platform, sizeof j->platform, json_str(v, "platform", ""));
    copy(j->version, sizeof j->version, json_str(v, "version", ""));
    copy(j->content_id, sizeof j->content_id, json_str(v, "contentId", ""));
    copy(j->cover, sizeof j->cover, json_str(v, "cover", ""));
    copy(j->drive, sizeof j->drive, json_str(v, "drive", "internal"));
    copy(j->state, sizeof j->state, json_str(v, "state", "queued"));
    copy(j->error, sizeof j->error, json_str(v, "error", ""));
    copy(j->note, sizeof j->note, json_str(v, "note", ""));
    copy(j->replace, sizeof j->replace, json_str(v, "replace", ""));
    j->total = (int64_t)json_num(v, "total", 0);
    j->done = (int64_t)json_num(v, "done", 0);
    j->created = (int64_t)json_num(v, "created", 0);
    j->started = (int64_t)json_num(v, "started", 0);
    j->finished = (int64_t)json_num(v, "finished", 0);
    jval_t *fs = json_get(v, "files");
    int n = 0;
    for (jval_t *f = fs ? fs->child : NULL; f; f = f->next) n++;
    j->files = calloc(n ? (size_t)n : 1, sizeof *j->files);
    for (jval_t *f = fs ? fs->child : NULL; f; f = f->next) {
      dfile_t *d = &j->files[j->nfiles++];
      copy(d->path, sizeof d->path, json_str(f, "path", ""));
      copy(d->url, sizeof d->url, json_str(f, "url", ""));
      copy(d->category, sizeof d->category, json_str(f, "category", ""));
      d->size = (int64_t)json_num(f, "size", 0);
      copy(d->dest, sizeof d->dest, json_str(f, "dest", ""));
      d->done = (int64_t)json_num(f, "done", 0);
    }
    /* A download interrupted by a restart or crash carries on by itself. */
    if (!strcmp(j->state, "downloading") || !strcmp(j->state, "verifying")) copy(j->state, sizeof j->state, "queued");
    int num = atoi(j->id + 1);
    if (num > id_counter) id_counter = num;
    jobs[njobs++] = j;
  }
  json_free(arr);
}

static job_t *find_locked(const char *id) {
  for (int i = 0; i < njobs; i++) if (!strcmp(jobs[i]->id, id)) return jobs[i];
  return NULL;
}

static void free_job(job_t *j) {
  free(j->files);
  free(j);
}

/* ---- speed limit while a game is running ------------------------------------------ */

static volatile int game_running;
static pthread_mutex_t rate_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t rate_window_start;
static int64_t rate_window_bytes;

static void throttle(size_t n, volatile int *stop) {
  if (!game_running) return;
  int limit = settings_get().play_limit_mbps;
  if (limit <= 0) return;
  int64_t allowed = (int64_t)limit * 1024 * 1024;
  for (;;) {
    if (*stop) return;
    pthread_mutex_lock(&rate_lock);
    int64_t now = now_ms();
    if (now - rate_window_start >= 1000) {
      rate_window_start = now;
      rate_window_bytes = 0;
    }
    if (rate_window_bytes + (int64_t)n <= allowed || rate_window_bytes == 0) {
      rate_window_bytes += (int64_t)n;
      pthread_mutex_unlock(&rate_lock);
      return;
    }
    int64_t wait = 1000 - (now - rate_window_start);
    pthread_mutex_unlock(&rate_lock);
    usleep((useconds_t)(wait > 100 ? 100 : wait > 0 ? wait : 1) * 1000);
  }
}

/* ---- one file, several connections ---------------------------------------------------- */

typedef struct {
  job_t *job;
  dfile_t *file;
  int fd;
  int nchunks;
  char *chunks;          /* '0' to do, 'p' in progress, '1' done */
  int *retries;
  char state_path[800];
  pthread_mutex_t mu;
  char error[256];
  int failed;
  int active;
} fctx_t;

typedef struct {
  fctx_t *fc;
  int64_t offset;   /* file offset of buf[0] */
  int64_t written;  /* bytes of this chunk received so far */
  char *buf;        /* WRITE_BUF bytes, written out when full */
  size_t buf_len;
} wctx_t;

static void write_failed(fctx_t *fc, int e) {
  pthread_mutex_lock(&fc->mu);
  if (!fc->failed) {
    snprintf(fc->error, sizeof fc->error, e == ENOSPC ? "The drive is full" : "Could not write to the drive (%s)", strerror(e));
    fc->failed = 1;
  }
  pthread_mutex_unlock(&fc->mu);
}

static int flush_buf(wctx_t *w) {
  size_t off = 0;
  while (off < w->buf_len) {
    ssize_t n = pwrite(w->fc->fd, w->buf + off, w->buf_len - off, w->offset + (int64_t)off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { write_failed(w->fc, n < 0 ? errno : EIO); return -1; }
    off += (size_t)n;
  }
  w->offset += (int64_t)w->buf_len;
  w->buf_len = 0;
  return 0;
}

static void save_chunks_locked(fctx_t *fc) {
  write_file_atomic(fc->state_path, fc->chunks, (size_t)fc->nchunks);
}

static int on_body(void *ctx, const char *data, size_t len) {
  wctx_t *w = ctx;
  if (w->fc->job->stop || w->fc->failed) return 1;
  throttle(len, &w->fc->job->stop);
  size_t off = 0;
  while (off < len) {
    size_t n = len - off;
    if (n > WRITE_BUF - w->buf_len) n = WRITE_BUF - w->buf_len;
    memcpy(w->buf + w->buf_len, data + off, n);
    w->buf_len += n;
    off += n;
    if (w->buf_len == WRITE_BUF && flush_buf(w)) return 1;
  }
  w->written += (int64_t)len;
  __atomic_add_fetch(&w->fc->job->done, (int64_t)len, __ATOMIC_RELAXED);
  __atomic_add_fetch(&w->fc->file->done, (int64_t)len, __ATOMIC_RELAXED);
  return 0;
}

static void *chunk_worker(void *arg) {
  fctx_t *fc = arg;
  char *buf = malloc(WRITE_BUF);
  if (!buf) {
    write_failed(fc, ENOMEM);
    pthread_mutex_lock(&fc->mu);
    fc->active--;
    pthread_mutex_unlock(&fc->mu);
    return NULL;
  }
  for (;;) {
    pthread_mutex_lock(&fc->mu);
    int c = -1;
    if (!fc->failed && !fc->job->stop) {
      for (int i = 0; i < fc->nchunks; i++) if (fc->chunks[i] == '0') { c = i; break; }
    }
    if (c >= 0) fc->chunks[c] = 'p';
    pthread_mutex_unlock(&fc->mu);
    if (c < 0) break;

    int64_t from = (int64_t)c * CHUNK;
    int64_t to = from + CHUNK - 1;
    if (to >= fc->file->size) to = fc->file->size - 1;
    wctx_t w = { fc, from, 0, buf, 0 };
    char err[256] = { 0 };
    http_info_t info;
    int rc = http_get(fc->file->url, from, to, on_body, &w, &info, &fc->job->stop, err, sizeof err);
    bool complete = rc == 0 && w.written == to - from + 1 && flush_buf(&w) == 0;

    pthread_mutex_lock(&fc->mu);
    if (complete) {
      fc->chunks[c] = '1';
      save_chunks_locked(fc);
    } else {
      /* Throw away what this attempt wrote; the chunk starts again. */
      __atomic_sub_fetch(&fc->job->done, w.written, __ATOMIC_RELAXED);
      __atomic_sub_fetch(&fc->file->done, w.written, __ATOMIC_RELAXED);
      fc->chunks[c] = '0';
      if (!fc->job->stop && !fc->failed) {
        if (++fc->retries[c] > MAX_RETRIES) {
          snprintf(fc->error, sizeof fc->error, "%s", err[0] ? err : "The download kept failing");
          fc->failed = 1;
        }
      }
    }
    bool retry_wait = !complete && !fc->failed && !fc->job->stop;
    pthread_mutex_unlock(&fc->mu);
    if (retry_wait) sleep(2);
  }
  free(buf);
  pthread_mutex_lock(&fc->mu);
  fc->active--;
  pthread_mutex_unlock(&fc->mu);
  return NULL;
}

static bool looks_like_exfat(const char *real) {
  int fd = open(real, O_RDONLY);
  if (fd < 0) return false;
  char head[16];
  ssize_t n = pread(fd, head, sizeof head, 0);
  close(fd);
  return n == (ssize_t)sizeof head && !memcmp(head + 3, "EXFAT   ", 8);
}

static bool ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcasecmp(s + a - b, suffix);
}

static void set_state(job_t *j, const char *state) {
  pthread_mutex_lock(&lock);
  copy(j->state, sizeof j->state, state);
  if (!strcmp(state, "done") || !strcmp(state, "failed")) j->finished = now_ms();
  save_locked();
  pthread_mutex_unlock(&lock);
}

static void fail(job_t *j, const char *msg) {
  pthread_mutex_lock(&lock);
  copy(j->error, sizeof j->error, msg);
  pthread_mutex_unlock(&lock);
  set_state(j, "failed");
  log_msg("download %s failed: %s", j->title, msg);
  if (settings_get().notify) platform_notify("GameVault: download failed", j->title);
}

/* 0 = finished, 1 = stopped (pause/cancel), -1 = failed (error set) */
static int download_file(job_t *j, dfile_t *f, char *err, size_t errlen) {
  char real[1100], part[1200];
  platform_path(f->dest, real, sizeof real);
  snprintf(part, sizeof part, "%s.part", real);
  if (file_size(real) == f->size) {
    if (f->done != f->size) {
      __atomic_add_fetch(&j->done, f->size - f->done, __ATOMIC_RELAXED);
      f->done = f->size;
    }
    return 0;
  }
  char dir[1100];
  copy(dir, sizeof dir, real);
  char *slash = strrchr(dir, '/');
  if (slash) { *slash = 0; mkdir_p(dir); }

  fctx_t fc;
  memset(&fc, 0, sizeof fc);
  pthread_mutex_init(&fc.mu, NULL);
  fc.job = j;
  fc.file = f;
  fc.nchunks = (int)((f->size + CHUNK - 1) / CHUNK);
  if (fc.nchunks == 0) fc.nchunks = 1;
  fc.chunks = malloc((size_t)fc.nchunks + 1);
  fc.retries = calloc((size_t)fc.nchunks, sizeof *fc.retries);
  snprintf(fc.state_path, sizeof fc.state_path, "%s.part.state", real);

  /* Resume: chunks already written are listed in the state file. */
  size_t slen = 0;
  char *saved = read_file(fc.state_path, &slen);
  bool resume = saved && slen == (size_t)fc.nchunks && file_exists(part);
  for (int i = 0; i < fc.nchunks; i++) fc.chunks[i] = resume && saved[i] == '1' ? '1' : '0';
  fc.chunks[fc.nchunks] = 0;
  free(saved);

  /* Recount progress from the chunks that are really there. */
  int64_t have = 0;
  for (int i = 0; i < fc.nchunks; i++) {
    if (fc.chunks[i] != '1') continue;
    int64_t len = CHUNK;
    if ((int64_t)(i + 1) * CHUNK > f->size) len = f->size - (int64_t)i * CHUNK;
    have += len;
  }
  __atomic_add_fetch(&j->done, have - f->done, __ATOMIC_RELAXED);
  f->done = have;

  fc.fd = open(part, O_RDWR | O_CREAT | (resume ? 0 : O_TRUNC), 0666);
  if (fc.fd < 0) {
    snprintf(err, errlen, "Could not create %s (%s)", f->dest, strerror(errno));
    free(fc.chunks);
    free(fc.retries);
    return -1;
  }
  if (ftruncate(fc.fd, f->size)) {
    snprintf(err, errlen, errno == ENOSPC || errno == EFBIG ? "Not enough space on the drive" : "Could not size the file (%s)", strerror(errno));
    close(fc.fd);
    free(fc.chunks);
    free(fc.retries);
    return -1;
  }
  pthread_mutex_lock(&fc.mu);
  save_chunks_locked(&fc);
  pthread_mutex_unlock(&fc.mu);

  settings_t cfg = settings_get();
  int conns = cfg.connections > 0 ? cfg.connections : cfg.measured_connections > 0 ? cfg.measured_connections : 2;
  if (conns > fc.nchunks) conns = fc.nchunks;
  if (conns < 1) conns = 1;
  pthread_t threads[8];
  int started = 0;
  for (int i = 0; i < conns; i++) {
    pthread_mutex_lock(&fc.mu);
    fc.active++;
    pthread_mutex_unlock(&fc.mu);
    if (pthread_create(&threads[started], NULL, chunk_worker, &fc) == 0) started++;
    else {
      pthread_mutex_lock(&fc.mu);
      fc.active--;
      pthread_mutex_unlock(&fc.mu);
    }
  }

  /* Watch progress: speed, game detection, saving the queue now and then. */
  int64_t last_bytes = j->done, last_t = now_ms(), last_save = last_t;
  for (;;) {
    pthread_mutex_lock(&fc.mu);
    int active = fc.active;
    pthread_mutex_unlock(&fc.mu);
    if (!active) break;
    usleep(500000);
    int64_t t = now_ms();
    if (t - last_t >= 1000) {
      int64_t d = j->done;
      double inst = (double)(d - last_bytes) * 1000.0 / (double)(t - last_t);
      j->speed = j->speed <= 0 ? inst : j->speed * 0.6 + inst * 0.4;
      last_bytes = d;
      last_t = t;
      game_running = platform_game_running();
    }
    if (t - last_save >= 5000) {
      pthread_mutex_lock(&lock);
      save_locked();
      pthread_mutex_unlock(&lock);
      last_save = t;
    }
  }
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  j->speed = 0;

  int result = 0;
  if (fc.failed) {
    snprintf(err, errlen, "%s", fc.error);
    result = -1;
  } else if (j->stop) {
    result = 1;
  } else {
    for (int i = 0; i < fc.nchunks; i++) if (fc.chunks[i] != '1') result = -1;
    if (result) snprintf(err, errlen, "The download did not finish");
  }
  fsync(fc.fd);
  close(fc.fd);
  free(fc.chunks);
  free(fc.retries);
  pthread_mutex_destroy(&fc.mu);
  if (result) return result;

  /* Check, then put the file in place. */
  set_state(j, "verifying");
  if (file_size(part) != f->size) {
    snprintf(err, errlen, "The downloaded file is the wrong size");
    return -1;
  }
  if (settings_get().check_files && ends_with(f->dest, ".exfat") && !looks_like_exfat(part)) {
    snprintf(err, errlen, "The downloaded file is not a valid exFAT image");
    unlink(part);
    unlink(fc.state_path);
    return -1;
  }
  if (rename(part, real)) {
    snprintf(err, errlen, "Could not rename the finished file (%s)", strerror(errno));
    return -1;
  }
  unlink(fc.state_path);
  pthread_mutex_lock(&lock);
  copy(j->state, sizeof j->state, "downloading");
  pthread_mutex_unlock(&lock);
  return 0;
}

static void delete_partials(job_t *j) {
  for (int i = 0; i < j->nfiles; i++) {
    char real[1100], p[1200];
    platform_path(j->files[i].dest, real, sizeof real);
    snprintf(p, sizeof p, "%s.part", real);
    unlink(p);
    snprintf(p, sizeof p, "%s.part.state", real);
    unlink(p);
  }
}

static void run_ps5(job_t *j) {
  drive_t d;
  if (!console_drive(j->drive, &d) || !d.present) {
    fail(j, "The download drive is not connected");
    return;
  }
  int64_t remaining = j->total - j->done;
  if (remaining > d.free_bytes) {
    char msg[160];
    snprintf(msg, sizeof msg, "Not enough space on %s: %.1f GB needed, %.1f GB free", d.label,
             remaining / 1e9, d.free_bytes / 1e9);
    fail(j, msg);
    return;
  }
  for (int i = 0; i < j->nfiles; i++) {
    char err[256] = { 0 };
    int rc = download_file(j, &j->files[i], err, sizeof err);
    if (rc == 1) {
      if (j->stop == 2) {
        delete_partials(j);
        pthread_mutex_lock(&lock);
        j->done = 0;
        for (int k = 0; k < j->nfiles; k++) j->files[k].done = 0;
        pthread_mutex_unlock(&lock);
        set_state(j, "cancelled");
      } else {
        /* stop == 3: GameVault is restarting; the new copy carries on. */
        set_state(j, j->stop == 3 ? "queued" : "paused");
      }
      return;
    }
    if (rc < 0) {
      fail(j, err);
      return;
    }
  }
  /* Update: remove the older copy now that the new one is in place. */
  if (j->replace[0] && strcmp(j->replace, j->files[0].dest)) {
    char err[200];
    console_rescan();
    if (console_delete(j->replace, err, sizeof err)) log_msg("could not remove old copy %s: %s", j->replace, err);
  }
  console_rescan();
  if (!strcmp(j->platform, "PS4")) {
    drive_t to;
    pthread_mutex_lock(&lock);
    snprintf(j->note, sizeof j->note, "Saved to the top folder of %s, ready to install from there.",
             console_drive(j->drive, &to) ? to.label : j->drive);
    pthread_mutex_unlock(&lock);
  }
  set_state(j, "done");
  log_msg("download %s finished", j->title);
  if (settings_get().notify) platform_notify("GameVault: download complete", j->title);
}

static int category_rank(const char *c) {
  if (!strncmp(c, "gd", 2)) return 0;
  if (!strcmp(c, "gp")) return 1;
  return 2;
}

static void run_ps4(job_t *j) {
  /* Base game, then updates, then add-ons: the PS5 installs them in order. */
  int sent = 0;
  for (int rank = 0; rank < 3; rank++) {
    for (int i = 0; i < j->nfiles; i++) {
      dfile_t *f = &j->files[i];
      if (category_rank(f->category) != rank) continue;
      if (j->stop) { set_state(j, j->stop == 2 ? "cancelled" : j->stop == 3 ? "queued" : "paused"); return; }
      char err[256] = { 0 };
      if (platform_install_pkg(f->url, j->content_id, j->title, j->cover, err, sizeof err)) {
        char msg[300];
        snprintf(msg, sizeof msg, "The PS5 installer refused %s: %s", f->path, err);
        fail(j, msg);
        return;
      }
      f->done = f->size;
      __atomic_add_fetch(&j->done, f->size, __ATOMIC_RELAXED);
      sent++;
    }
  }
  pthread_mutex_lock(&lock);
  snprintf(j->note, sizeof j->note, "Sent %d package%s to the PS5's installer. Progress shows in the console's Downloads.", sent, sent == 1 ? "" : "s");
  pthread_mutex_unlock(&lock);
  set_state(j, "done");
  console_rescan();
  if (settings_get().notify) platform_notify("GameVault: installing", j->title);
}

static void *worker(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&lock);
    job_t *j = NULL;
    while (!j) {
      for (int i = 0; i < njobs && !halted; i++) {
        if (!strcmp(jobs[i]->state, "queued")) { j = jobs[i]; break; }
      }
      if (!j) pthread_cond_wait(&wake, &lock);
    }
    j->stop = 0;
    j->error[0] = 0;
    copy(j->state, sizeof j->state, "downloading");
    if (!j->started) j->started = now_ms();
    save_locked();
    pthread_mutex_unlock(&lock);

    game_running = platform_game_running();
    if (!strcmp(j->platform, "PS4") && !strcmp(j->drive, "internal")) run_ps4(j);
    else run_ps5(j); /* PS5 games, and PS4 .pkg files copied to a USB drive */
  }
  return NULL;
}

void downloads_init(void) {
  pthread_mutex_lock(&lock);
  load();
  pthread_mutex_unlock(&lock);
  pthread_t t;
  if (pthread_create(&t, NULL, worker, NULL) == 0) pthread_detach(t);
}

/* ---- adding jobs ----------------------------------------------------------------------- */

static void url_path_encode(sbuf_t *b, const char *path) {
  static const char *hex = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~/", *p)) {
      sbuf_append(b, p, 1);
    } else {
      char e[3] = { '%', hex[*p >> 4], hex[*p & 15] };
      sbuf_append(b, e, 3);
    }
  }
}

/* Folder-safe name: drops characters the PS5's file systems dislike. */
static void clean_name(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const char *p = in; *p && o + 1 < n; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 32 || strchr("/\\:*?\"<>|", c)) continue;
    out[o++] = (char)c;
  }
  while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
  out[o] = 0;
  if (!o || !strcmp(out, "..")) snprintf(out, n, "game");
}

static bool selected(const jval_t *wanted, const char *path) {
  if (!wanted) return true;
  for (jval_t *w = wanted->child; w; w = w->next) if (w->type == J_STR && !strcmp(w->s, path)) return true;
  return false;
}

static bool rel_path_ok(const char *p) {
  if (!*p || *p == '/') return false;
  const char *s = p;
  while (*s) {
    const char *e = strchr(s, '/');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n == 0 || (n == 2 && !strncmp(s, "..", 2)) || (n == 1 && *s == '.')) return false;
    s = e ? e + 1 : s + n;
  }
  return true;
}

int downloads_add(int64_t game_id, const char *files_json, const char *replace_path, const char *drive,
                  char *id_out, size_t idlen, char *err, size_t errlen) {
  settings_t s = settings_get();
  if (!s.server[0]) {
    snprintf(err, errlen, "Set the GameVault address in Settings first");
    return -1;
  }
  char url[600];
  sbuf_t body;
  int status = 0;

  snprintf(url, sizeof url, "%s/api/v1/games/%lld", s.server, (long long)game_id);
  sbuf_init(&body);
  if (http_get_text(url, &body, &status, err, errlen)) { sbuf_free(&body); return -1; }
  jval_t *game = json_parse(body.data);
  sbuf_free(&body);
  snprintf(url, sizeof url, "%s/api/v1/games/%lld/files", s.server, (long long)game_id);
  sbuf_init(&body);
  if (http_get_text(url, &body, &status, err, errlen)) { sbuf_free(&body); json_free(game); return -1; }
  jval_t *files = json_parse(body.data);
  sbuf_free(&body);
  jval_t *wanted = files_json && *files_json ? json_parse(files_json) : NULL;
  if (wanted && wanted->type != J_ARR) { json_free(wanted); wanted = NULL; }

  int rc = -1;
  job_t *j = NULL;
  if (!game || !files || game->type != J_OBJ) {
    snprintf(err, errlen, "GameVault sent an unexpected answer");
    goto out;
  }
  const char *kind = json_str(files, "kind", "");
  const char *platform = json_str(game, "platform", "PS5");
  if (!strcmp(kind, "ps4-folder")) {
    snprintf(err, errlen, "PS4 folder dumps can't be installed over the network. Make a .pkg of it first.");
    goto out;
  }
  jval_t *list = json_get(files, "files");
  int n = 0;
  for (jval_t *f = list ? list->child : NULL; f; f = f->next) if (selected(wanted, json_str(f, "path", ""))) n++;
  if (!n) {
    snprintf(err, errlen, "There are no files to download for this game");
    goto out;
  }

  j = calloc(1, sizeof *j);
  j->files = calloc((size_t)n, sizeof *j->files);
  j->game_id = game_id;
  copy(j->title, sizeof j->title, json_str(game, "title", "Game"));
  copy(j->title_id, sizeof j->title_id, json_str(game, "titleId", ""));
  copy(j->platform, sizeof j->platform, platform);
  copy(j->version, sizeof j->version, json_str(game, "version", ""));
  copy(j->content_id, sizeof j->content_id, json_str(game, "contentId", ""));
  const char *cover = json_str(game, "cover", "");
  if (cover[0] == '/') snprintf(j->cover, sizeof j->cover, "%s%s", s.server, cover);
  else copy(j->cover, sizeof j->cover, cover);
  const bool ps4 = !strcmp(platform, "PS4");
  /* PS4: "internal" sends the .pkg files to the PS5's installer; a USB drive gets the
     files themselves, in its top folder, to install from later. */
  const char *target = drive && *drive ? drive : ps4 ? s.ps4_drive : s.ps5_drive;
  if (ps4 && strcmp(target, "internal") && strncmp(target, "usb", 3)) target = "internal";
  copy(j->drive, sizeof j->drive, target);
  const bool ps4_copy = ps4 && strcmp(j->drive, "internal");
  copy(j->state, sizeof j->state, "queued");
  /* PS5: the older copy is deleted once the new one is in. A PS4 update adds its
     patch beside the game, so nothing is deleted. */
  copy(j->replace, sizeof j->replace, strcmp(platform, "PS4") ? replace_path : "");
  j->created = now_ms();

  drive_t d;
  if (!console_drive(j->drive, &d) || ((!ps4 || ps4_copy) && !d.present)) {
    snprintf(err, errlen, "That drive is not connected to the PS5");
    goto out;
  }
  char folder[256];
  const char *gpath = json_str(game, "path", "");
  const char *base = strrchr(gpath, '/');
  clean_name(base ? base + 1 : (j->title_id[0] ? j->title_id : j->title), folder, sizeof folder);

  for (jval_t *f = list->child; f; f = f->next) {
    const char *path = json_str(f, "path", "");
    if (!selected(wanted, path)) continue;
    if (!rel_path_ok(path)) {
      snprintf(err, errlen, "GameVault listed an unsafe file path: %s", path);
      goto out;
    }
    dfile_t *df = &j->files[j->nfiles++];
    copy(df->path, sizeof df->path, path);
    copy(df->category, sizeof df->category, json_str(f, "category", ""));
    df->size = (int64_t)json_num(f, "size", 0);
    sbuf_t u;
    sbuf_init(&u);
    const char *furl = json_str(f, "url", NULL);
    sbuf_puts(&u, s.server);
    if (furl) sbuf_puts(&u, furl);
    else {
      sbuf_printf(&u, "/api/v1/games/%lld/files/", (long long)game_id);
      url_path_encode(&u, path);
    }
    copy(df->url, sizeof df->url, u.data);
    sbuf_free(&u);
    if (!strcmp(kind, "ps5-exfat")) {
      const char *leaf = strrchr(path, '/');
      char name[256];
      clean_name(leaf ? leaf + 1 : path, name, sizeof name);
      snprintf(df->dest, sizeof df->dest, "%s/%s", d.homebrew, name);
    } else if (!strcmp(platform, "PS5")) {
      snprintf(df->dest, sizeof df->dest, "%s/%s/%s", d.homebrew, folder, path);
    } else if (ps4_copy) {
      const char *leaf = strrchr(path, '/');
      char name[256];
      clean_name(leaf ? leaf + 1 : path, name, sizeof name);
      snprintf(df->dest, sizeof df->dest, "%s/%s", d.root, name);
    }
    j->total += df->size;
  }

  pthread_mutex_lock(&lock);
  /* One job per game at a time. */
  for (int i = 0; i < njobs; i++) {
    job_t *o = jobs[i];
    if (o->game_id == game_id && (!strcmp(o->state, "queued") || !strcmp(o->state, "downloading") ||
                                  !strcmp(o->state, "paused") || !strcmp(o->state, "verifying"))) {
      pthread_mutex_unlock(&lock);
      snprintf(err, errlen, "This game is already in the download queue");
      goto out;
    }
  }
  if (njobs >= MAX_JOBS) {
    /* Make room by dropping the oldest finished job. */
    for (int i = 0; i < njobs; i++) {
      if (!strcmp(jobs[i]->state, "done") || !strcmp(jobs[i]->state, "cancelled") || !strcmp(jobs[i]->state, "failed")) {
        free_job(jobs[i]);
        memmove(&jobs[i], &jobs[i + 1], (size_t)(njobs - i - 1) * sizeof *jobs);
        njobs--;
        break;
      }
    }
  }
  if (njobs >= MAX_JOBS) {
    pthread_mutex_unlock(&lock);
    snprintf(err, errlen, "The download list is full");
    goto out;
  }
  snprintf(j->id, sizeof j->id, "d%d", ++id_counter);
  jobs[njobs++] = j;
  save_locked();
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&lock);
  copy(id_out, idlen, j->id);
  j = NULL;
  rc = 0;

out:
  if (j) free_job(j);
  json_free(game);
  json_free(files);
  json_free(wanted);
  return rc;
}

/* ---- controls ---------------------------------------------------------------------------- */

static bool active_state(const char *s) { return !strcmp(s, "downloading") || !strcmp(s, "verifying"); }

int downloads_pause(const char *id, char *err, size_t errlen) {
  pthread_mutex_lock(&lock);
  job_t *j = find_locked(id);
  int rc = 0;
  if (!j) { snprintf(err, errlen, "No such download"); rc = -1; }
  else if (active_state(j->state)) j->stop = 1;
  else if (!strcmp(j->state, "queued")) { copy(j->state, sizeof j->state, "paused"); save_locked(); }
  else { snprintf(err, errlen, "This download can't be paused now"); rc = -1; }
  pthread_mutex_unlock(&lock);
  return rc;
}

int downloads_resume(const char *id, char *err, size_t errlen) {
  pthread_mutex_lock(&lock);
  job_t *j = find_locked(id);
  int rc = 0;
  if (!j) { snprintf(err, errlen, "No such download"); rc = -1; }
  else if (!strcmp(j->state, "paused") || !strcmp(j->state, "failed") || !strcmp(j->state, "cancelled")) {
    copy(j->state, sizeof j->state, "queued");
    j->error[0] = 0;
    j->finished = 0;
    save_locked();
    pthread_cond_signal(&wake);
  } else { snprintf(err, errlen, "This download is not paused"); rc = -1; }
  pthread_mutex_unlock(&lock);
  return rc;
}

int downloads_cancel(const char *id, char *err, size_t errlen) {
  pthread_mutex_lock(&lock);
  job_t *j = find_locked(id);
  int rc = 0;
  bool cleanup = false;
  if (!j) { snprintf(err, errlen, "No such download"); rc = -1; }
  else if (active_state(j->state)) j->stop = 2;
  else if (!strcmp(j->state, "queued") || !strcmp(j->state, "paused") || !strcmp(j->state, "failed")) {
    copy(j->state, sizeof j->state, "cancelled");
    j->done = 0;
    for (int k = 0; k < j->nfiles; k++) j->files[k].done = 0;
    cleanup = strcmp(j->platform, "PS4") != 0;
    save_locked();
  } else { snprintf(err, errlen, "This download can't be cancelled now"); rc = -1; }
  pthread_mutex_unlock(&lock);
  if (cleanup) delete_partials(j);
  return rc;
}

int downloads_remove(const char *id, char *err, size_t errlen) {
  pthread_mutex_lock(&lock);
  int rc = -1;
  bool found = false;
  for (int i = 0; i < njobs; i++) {
    if (strcmp(jobs[i]->id, id)) continue;
    found = true;
    const char *s = jobs[i]->state;
    if (strcmp(s, "done") && strcmp(s, "cancelled") && strcmp(s, "failed")) {
      snprintf(err, errlen, "Cancel the download before removing it");
      break;
    }
    bool partials = !strcmp(s, "failed") && strcmp(jobs[i]->platform, "PS4");
    job_t *j = jobs[i];
    memmove(&jobs[i], &jobs[i + 1], (size_t)(njobs - i - 1) * sizeof *jobs);
    njobs--;
    save_locked();
    if (partials) delete_partials(j);
    free_job(j);
    rc = 0;
    break;
  }
  if (!found) snprintf(err, errlen, "No such download");
  pthread_mutex_unlock(&lock);
  return rc;
}

int downloads_move(const char *id, int delta, char *err, size_t errlen) {
  pthread_mutex_lock(&lock);
  int rc = -1;
  for (int i = 0; i < njobs; i++) {
    if (strcmp(jobs[i]->id, id)) continue;
    int k = i + (delta < 0 ? -1 : 1);
    if (k >= 0 && k < njobs) {
      job_t *t = jobs[i];
      jobs[i] = jobs[k];
      jobs[k] = t;
      save_locked();
    }
    rc = 0;
    break;
  }
  if (rc) snprintf(err, errlen, "No such download");
  pthread_mutex_unlock(&lock);
  return rc;
}

void downloads_clear_finished(void) {
  pthread_mutex_lock(&lock);
  int o = 0;
  for (int i = 0; i < njobs; i++) {
    if (!strcmp(jobs[i]->state, "done") || !strcmp(jobs[i]->state, "cancelled")) free_job(jobs[i]);
    else jobs[o++] = jobs[i];
  }
  njobs = o;
  save_locked();
  pthread_mutex_unlock(&lock);
}

void downloads_json(sbuf_t *out) {
  pthread_mutex_lock(&lock);
  sbuf_puts(out, "[");
  for (int i = 0; i < njobs; i++) {
    if (i) sbuf_puts(out, ",");
    job_json(jobs[i], out, false);
  }
  sbuf_puts(out, "]");
  pthread_mutex_unlock(&lock);
}

int64_t downloads_reserved(const char *drive) {
  int64_t n = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < njobs; i++) {
    job_t *j = jobs[i];
    if (strcmp(j->drive, drive) || !strcmp(j->platform, "PS4")) continue;
    if (!strcmp(j->state, "queued") || !strcmp(j->state, "paused") || active_state(j->state)) n += j->total - j->done;
  }
  pthread_mutex_unlock(&lock);
  return n;
}

bool downloads_busy(void) {
  bool busy = false;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < njobs; i++) if (active_state(jobs[i]->state) && strcmp(jobs[i]->platform, "PS4")) busy = true;
  pthread_mutex_unlock(&lock);
  return busy;
}

void downloads_prepare_restart(void) {
  pthread_mutex_lock(&lock);
  halted = 1;
  for (int i = 0; i < njobs; i++) if (active_state(jobs[i]->state)) jobs[i]->stop = 3;
  pthread_mutex_unlock(&lock);
  /* Give the running download a few seconds to save where it got to. */
  for (int t = 0; t < 100; t++) {
    bool busy = false;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < njobs; i++) if (active_state(jobs[i]->state)) busy = true;
    if (!busy) save_locked();
    pthread_mutex_unlock(&lock);
    if (!busy) return;
    usleep(100000);
  }
}

void downloads_cancel_restart(void) {
  pthread_mutex_lock(&lock);
  halted = 0;
  pthread_cond_signal(&wake);
  pthread_mutex_unlock(&lock);
}
