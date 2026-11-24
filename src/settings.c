#include "settings.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "platform.h"

static settings_t current;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void defaults(settings_t *s) {
  memset(s, 0, sizeof *s);
  snprintf(s->ps5_drive, sizeof s->ps5_drive, "internal");
  s->connections = 4;
  s->play_limit_mbps = 0;
  s->notify = true;
  s->check_files = true;
  snprintf(s->transfer, sizeof s->transfer, "auto");
}

static void path(char *out, size_t n) { snprintf(out, n, "%s/settings.json", platform_data_dir()); }

static bool valid_drive(const char *d) {
  if (!strcmp(d, "internal")) return true;
  if (!strncmp(d, "usb", 3) && d[3] >= '0' && d[3] <= '7' && !d[4]) return true;
  if (!strncmp(d, "ext", 3) && d[3] >= '0' && d[3] <= '3' && !d[4]) return true;
  return false;
}

const char *settings_apply_json(settings_t *s, const char *json) {
  jval_t *v = json_parse(json);
  if (!v || v->type != J_OBJ) { json_free(v); return "Settings must be a JSON object"; }
  const char *err = NULL;
  const char *server = json_str(v, "server", NULL);
  if (server) {
    url_t u;
    size_t len = strlen(server);
    if (len && !url_parse(server, &u)) err = "The GameVault address must start with http://";
    else if (len >= sizeof s->server) err = "The GameVault address is too long";
    else {
      snprintf(s->server, sizeof s->server, "%s", server);
      while ((len = strlen(s->server)) && s->server[len - 1] == '/') s->server[len - 1] = 0;
    }
  }
  const char *drive = json_str(v, "ps5Drive", NULL);
  if (drive) {
    if (valid_drive(drive)) snprintf(s->ps5_drive, sizeof s->ps5_drive, "%s", drive);
    else err = "Unknown drive";
  }
  jval_t *c = json_get(v, "connections");
  if (c && c->type == J_NUM) s->connections = c->n < 1 ? 1 : c->n > 8 ? 8 : (int)c->n;
  jval_t *l = json_get(v, "playLimitMBps");
  if (l && l->type == J_NUM) s->play_limit_mbps = l->n < 0 ? 0 : l->n > 1000 ? 1000 : (int)l->n;
  jval_t *nt = json_get(v, "notify");
  if (nt && nt->type == J_BOOL) s->notify = nt->b;
  jval_t *cf = json_get(v, "checkFiles");
  if (cf && cf->type == J_BOOL) s->check_files = cf->b;
  const char *tr = json_str(v, "transfer", NULL);
  if (tr) {
    if (!strcmp(tr, "auto") || !strcmp(tr, "http") || !strcmp(tr, "nfs")) snprintf(s->transfer, sizeof s->transfer, "%s", tr);
    else err = "Unknown transfer method";
  }
  const char *nfs = json_str(v, "nfsExport", NULL);
  if (nfs) snprintf(s->nfs_export, sizeof s->nfs_export, "%s", nfs);
  json_free(v);
  return err;
}

void settings_to_json(const settings_t *s, sbuf_t *out) {
  sbuf_puts(out, "{");
  jw_kv_str(out, "server", s->server, false);
  jw_kv_str(out, "ps5Drive", s->ps5_drive, true);
  jw_kv_int(out, "connections", s->connections, true);
  jw_kv_int(out, "playLimitMBps", s->play_limit_mbps, true);
  jw_kv_bool(out, "notify", s->notify, true);
  jw_kv_bool(out, "checkFiles", s->check_files, true);
  jw_kv_str(out, "transfer", s->transfer, true);
  jw_kv_str(out, "nfsExport", s->nfs_export, true);
  sbuf_puts(out, "}");
}

void settings_load(void) {
  settings_t s;
  defaults(&s);
  char p[600];
  path(p, sizeof p);
  char *text = read_file(p, NULL);
  if (text) {
    const char *err = settings_apply_json(&s, text);
    if (err) log_msg("settings.json: %s", err);
    free(text);
  }
  pthread_mutex_lock(&lock);
  current = s;
  pthread_mutex_unlock(&lock);
}

void settings_save(void) {
  settings_t s = settings_get();
  sbuf_t b;
  sbuf_init(&b);
  settings_to_json(&s, &b);
  char p[600];
  path(p, sizeof p);
  mkdir_p(platform_data_dir());
  if (write_file_atomic(p, b.data, b.len)) log_msg("could not save %s", p);
  sbuf_free(&b);
}

settings_t settings_get(void) {
  pthread_mutex_lock(&lock);
  settings_t s = current;
  pthread_mutex_unlock(&lock);
  return s;
}

void settings_set(const settings_t *s) {
  pthread_mutex_lock(&lock);
  current = *s;
  pthread_mutex_unlock(&lock);
  settings_save();
}
