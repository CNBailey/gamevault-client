/* REST API for the interface (TV browser, phone or PC) plus the static UI. */
#include "api.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "assets.h"
#include "console.h"
#include "downloads.h"
#include "http_client.h"
#include "json.h"
#include "platform.h"
#include "settings.h"
#include "speedtest.h"
#include "tile.h"

#ifndef GV_VERSION
#define GV_VERSION "dev"
#endif

int api_port(void);

static bool is(const http_req_t *r, const char *method, const char *path) {
  return !strcmp(r->method, method) && !strcmp(r->path, path);
}

/* "/api/downloads/d12/pause" -> id "d12", action "pause" */
static bool download_route(const char *path, char *id, size_t idlen, char *action, size_t alen) {
  const char *prefix = "/api/downloads/";
  size_t pl = strlen(prefix);
  if (strncmp(path, prefix, pl)) return false;
  const char *p = path + pl;
  const char *slash = strchr(p, '/');
  size_t n = slash ? (size_t)(slash - p) : strlen(p);
  if (!n || n >= idlen) return false;
  memcpy(id, p, n);
  id[n] = 0;
  snprintf(action, alen, "%s", slash ? slash + 1 : "");
  return true;
}

static void ok(http_res_t *res) {
  sbuf_t b;
  sbuf_init(&b);
  sbuf_puts(&b, "{\"ok\":true}");
  res_json(res, 200, &b);
}

static void state(http_res_t *res) {
  settings_t s = settings_get();
  char fw[32];
  platform_firmware(fw, sizeof fw);
  sbuf_t b;
  sbuf_init(&b);
  sbuf_puts(&b, "{");
  jw_kv_str(&b, "version", GV_VERSION, false);
  jw_kv_str(&b, "firmware", fw, true);
  jw_kv_bool(&b, "gameRunning", platform_game_running(), true);
  sbuf_puts(&b, ",\"settings\":");
  settings_to_json(&s, &b);
  sbuf_puts(&b, ",\"drives\":");
  console_drives_json(&b);
  sbuf_puts(&b, ",\"reserved\":{");
  drive_t d[MAX_DRIVES];
  int n = console_drives(d);
  bool first = true;
  for (int i = 0; i < n; i++) {
    if (!d[i].present) continue;
    jw_kv_int(&b, d[i].id, downloads_reserved(d[i].id), !first);
    first = false;
  }
  sbuf_puts(&b, "},\"tile\":");
  tile_json(&b);
  sbuf_puts(&b, ",\"installed\":");
  console_installed_json(&b);
  sbuf_puts(&b, ",\"downloads\":");
  downloads_json(&b);
  sbuf_puts(&b, "}");
  res_json(res, 200, &b);
}

static void test_connection(const http_req_t *req, http_res_t *res) {
  settings_t s = settings_get();
  char server[256];
  snprintf(server, sizeof server, "%s", s.server);
  jval_t *body = json_parse(req->body_len ? req->body : "{}");
  const char *override = json_str(body, "server", NULL);
  if (override && *override) snprintf(server, sizeof server, "%s", override);
  json_free(body);
  size_t len = strlen(server);
  while (len && server[len - 1] == '/') server[--len] = 0;
  if (!server[0]) { res_error(res, 400, "Enter the GameVault address first"); return; }

  char url[400], err[256] = { 0 };
  snprintf(url, sizeof url, "%s/api/v1/health", server);
  sbuf_t text;
  sbuf_init(&text);
  int status = 0;
  int64_t t0 = now_ms();
  int rc = http_get_text(url, &text, &status, err, sizeof err);
  int64_t ms = now_ms() - t0;
  sbuf_t b;
  sbuf_init(&b);
  sbuf_puts(&b, "{");
  if (rc) {
    jw_kv_bool(&b, "ok", false, false);
    jw_kv_str(&b, "error", err, true);
  } else {
    jval_t *h = json_parse(text.data);
    jw_kv_bool(&b, "ok", h != NULL, false);
    jw_kv_int(&b, "latencyMs", ms, true);
    jw_kv_str(&b, "serverVersion", json_str(h, "version", ""), true);
    jw_kv_int(&b, "games", (int64_t)json_num(h, "games", 0), true);
    if (!h) jw_kv_str(&b, "error", "That address answered, but it is not GameVault", true);
    json_free(h);
  }
  sbuf_puts(&b, "}");
  sbuf_free(&text);
  res_json(res, 200, &b);
}

static void log_line(const char *line, void *ctx) {
  sbuf_t *b = ctx;
  if (b->len > 1) sbuf_puts(b, ",");
  jw_str(b, line);
}

static void *quit_later(void *arg) {
  (void)arg;
  usleep(300000);
  log_msg("stopping (asked by the interface)");
  _exit(0);
  return NULL;
}

static void api(const http_req_t *req, http_res_t *res) {
  char err[300] = { 0 };

  if (is(req, "GET", "/api/state")) { state(res); return; }

  if (is(req, "GET", "/api/settings")) {
    settings_t s = settings_get();
    sbuf_t b;
    sbuf_init(&b);
    settings_to_json(&s, &b);
    res_json(res, 200, &b);
    return;
  }
  if (is(req, "PUT", "/api/settings")) {
    settings_t s = settings_get();
    const char *e = settings_apply_json(&s, req->body);
    if (e) { res_error(res, 400, e); return; }
    settings_set(&s);
    sbuf_t b;
    sbuf_init(&b);
    settings_to_json(&s, &b);
    res_json(res, 200, &b);
    return;
  }
  if (is(req, "POST", "/api/test-connection")) { test_connection(req, res); return; }
  if (is(req, "POST", "/api/rescan")) { console_rescan(); ok(res); return; }
  if (is(req, "GET", "/api/log")) {
    sbuf_t b;
    sbuf_init(&b);
    sbuf_puts(&b, "[");
    log_lines(log_line, &b);
    sbuf_puts(&b, "]");
    res_json(res, 200, &b);
    return;
  }
  if (is(req, "GET", "/api/diagnostics")) {
    /* The end of a log file: the native app's own, or ShadowMountPlus's. */
    char which[16] = "app";
    query_param(req->query, "file", which, sizeof which);
    const char *path = !strcmp(which, "smp") ? "/data/shadowmount/debug.log"
                     : !strcmp(which, "app1") ? "/data/gamevault/app.log.1" : "/data/gamevault/app.log";
    char real[600];
    platform_path(path, real, sizeof real);
    size_t len = 0;
    char *text = read_file(real, &len);
    res->status = 200;
    res->content_type = "text/plain; charset=utf-8";
    if (!text) {
      static const char none[] = "(no log yet)\n";
      res->body = none;
      res->body_len = sizeof none - 1;
      return;
    }
    size_t keep = len > 96 * 1024 ? 96 * 1024 : len;
    memmove(text, text + len - keep, keep);
    res->owned = text;
    res->body = text;
    res->body_len = keep;
    return;
  }
  if (is(req, "GET", "/api/resolve")) {
    /* The home-screen app can't look names up itself (getaddrinfo is in a
       WebKit library a game can't load), so it asks the service. */
    char host[256] = { 0 }, ip[INET_ADDRSTRLEN] = { 0 };
    if (!query_param(req->query, "host", host, sizeof host)) { res_error(res, 400, "host is required"); return; }
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) != 1) {
      struct addrinfo hints, *found = NULL;
      memset(&hints, 0, sizeof hints);
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      if (getaddrinfo(host, NULL, &hints, &found) || !found) { res_error(res, 404, "not found"); return; }
      a = ((struct sockaddr_in *)found->ai_addr)->sin_addr;
      freeaddrinfo(found);
    }
    inet_ntop(AF_INET, &a, ip, sizeof ip);
    sbuf_t b;
    sbuf_init(&b);
    sbuf_puts(&b, "{\"ip\":\"");
    sbuf_puts(&b, ip);
    sbuf_puts(&b, "\"}");
    res_json(res, 200, &b);
    return;
  }
  if (is(req, "POST", "/api/open")) { tile_open_later(); ok(res); return; }
  if (is(req, "POST", "/api/update-check")) { tile_check_now(); ok(res); return; }
  if (is(req, "POST", "/api/update")) {
    int rc = tile_update_now(err, sizeof err);   /* only returns when it failed */
    if (rc == -2 && http_server_start(api_port(), api_handle)) log_msg("could not listen again after a failed update");
    res_error(res, 409, err);
    return;
  }
  if (is(req, "POST", "/api/speed-test")) {
    jval_t *b = json_parse(req->body);
    int64_t game_id = (int64_t)json_num(b, "gameId", 0);
    json_free(b);
    sbuf_t o;
    sbuf_init(&o);
    if (game_id <= 0) { sbuf_free(&o); res_error(res, 400, "gameId is required"); return; }
    if (speedtest_run(game_id, &o, err, sizeof err)) { sbuf_free(&o); res_error(res, 409, err); return; }
    res_json(res, 200, &o);
    return;
  }
  if (is(req, "POST", "/api/notify-test")) {
    platform_notify("GameVault", "Notifications are working");
    ok(res);
    return;
  }
  if (is(req, "POST", "/api/quit")) {
    pthread_t t;
    if (pthread_create(&t, NULL, quit_later, NULL) == 0) pthread_detach(t);
    ok(res);
    return;
  }

  if (is(req, "DELETE", "/api/installed")) {
    char path[600];
    if (!query_param(req->query, "path", path, sizeof path)) { res_error(res, 400, "path is required"); return; }
    if (console_delete(path, err, sizeof err)) { res_error(res, 409, err); return; }
    ok(res);
    return;
  }

  if (is(req, "POST", "/api/downloads")) {
    jval_t *b = json_parse(req->body);
    if (!b || b->type != J_OBJ) { json_free(b); res_error(res, 400, "Send a JSON object"); return; }
    int64_t game_id = (int64_t)json_num(b, "gameId", 0);
    const char *replace = json_str(b, "replace", "");
    const char *drive = json_str(b, "drive", "");
    char *files = NULL;
    jval_t *fl = json_get(b, "files");
    if (fl && fl->type == J_ARR) {
      sbuf_t fb;
      sbuf_init(&fb);
      sbuf_puts(&fb, "[");
      for (jval_t *f = fl->child; f; f = f->next) {
        if (f->type != J_STR) continue;
        if (fb.len > 1) sbuf_puts(&fb, ",");
        jw_str(&fb, f->s);
      }
      sbuf_puts(&fb, "]");
      files = sbuf_detach(&fb);
    }
    char id[24];
    int rc = game_id > 0 ? downloads_add(game_id, files, replace, drive, id, sizeof id, err, sizeof err) : -1;
    if (game_id <= 0) snprintf(err, sizeof err, "gameId is required");
    free(files);
    json_free(b);
    if (rc) { res_error(res, 409, err); return; }
    sbuf_t o;
    sbuf_init(&o);
    sbuf_puts(&o, "{");
    jw_kv_str(&o, "id", id, false);
    sbuf_puts(&o, "}");
    res_json(res, 201, &o);
    return;
  }
  if (is(req, "GET", "/api/downloads")) {
    sbuf_t b;
    sbuf_init(&b);
    downloads_json(&b);
    res_json(res, 200, &b);
    return;
  }
  if (is(req, "POST", "/api/downloads/clear")) { downloads_clear_finished(); ok(res); return; }

  char id[24], action[32];
  if (download_route(req->path, id, sizeof id, action, sizeof action)) {
    int rc;
    if (!strcmp(req->method, "DELETE") && !action[0]) rc = downloads_remove(id, err, sizeof err);
    else if (strcmp(req->method, "POST")) { res_error(res, 405, "Use POST"); return; }
    else if (!strcmp(action, "pause")) rc = downloads_pause(id, err, sizeof err);
    else if (!strcmp(action, "resume")) rc = downloads_resume(id, err, sizeof err);
    else if (!strcmp(action, "cancel")) rc = downloads_cancel(id, err, sizeof err);
    else if (!strcmp(action, "up")) rc = downloads_move(id, -1, err, sizeof err);
    else if (!strcmp(action, "down")) rc = downloads_move(id, 1, err, sizeof err);
    else { res_error(res, 404, "Unknown action"); return; }
    if (rc) { res_error(res, 409, err); return; }
    ok(res);
    return;
  }

  res_error(res, 404, "Not found");
}

static void serve_static(const http_req_t *req, http_res_t *res) {
  if (strcmp(req->method, "GET") && strcmp(req->method, "HEAD")) { res_error(res, 405, "Method not allowed"); return; }
  const asset_t *a = asset_find(req->path);
  /* Unknown paths get the app (client-side routes). */
  if (!a && !strchr(strrchr(req->path, '/') ? strrchr(req->path, '/') : req->path, '.')) a = asset_find("/index.html");
  if (!a) { res_error(res, 404, "Not found"); return; }
  res->status = 200;
  res->content_type = a->mime;
  res->body = a->data;
  res->body_len = a->len;
  res->extra_headers = a->gzip
    ? (!strcmp(a->path, "/index.html") ? "Content-Encoding: gzip\r\nCache-Control: no-cache\r\n" : "Content-Encoding: gzip\r\nCache-Control: max-age=31536000, immutable\r\n")
    : (!strcmp(a->path, "/index.html") ? "Cache-Control: no-cache\r\n" : "Cache-Control: max-age=86400\r\n");
  if (!strcmp(req->method, "HEAD")) res->body_len = 0;
}

static int port_in_use = 8687;
void api_set_port(int port) { port_in_use = port; }
int api_port(void) { return port_in_use; }

void api_handle(const http_req_t *req, http_res_t *res) {
  if (!strncmp(req->path, "/api/", 5)) api(req, res);
  else serve_static(req, res);
}
