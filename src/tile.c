#include "tile.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "assets.h"
#include "downloads.h"
#include "http_client.h"
#include "http_server.h"
#include "json.h"
#include "platform.h"
#include "settings.h"

#ifndef GV_VERSION
#define GV_VERSION "dev"
#endif

extern const asset_t tile_assets[];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool installed;
static char install_error[200];
static char elf_version[32];     /* version of gamevault.elf kept next to the tile */
static char latest_version[32];  /* newest version GameVault offers */
static char check_error[200];
static int64_t checked_at;
static volatile int check_requested;

/* Compares "0.1.10" style versions by number. */
static int compare_versions(const char *a, const char *b) {
  for (;;) {
    while (*a && (*a < '0' || *a > '9')) a++;
    while (*b && (*b < '0' || *b > '9')) b++;
    if (!*a && !*b) return 0;
    char *ea, *eb;
    long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
    if (x != y) return x < y ? -1 : 1;
    a = ea;
    b = eb;
  }
}

static bool newer_than_running(const char *v) {
  if (!v[0]) return false;
  if (!strcmp(GV_VERSION, "dev")) return true;
  return compare_versions(v, GV_VERSION) > 0;
}

static void real_path(const char *rel, char *out, size_t n) {
  char console[600];
  snprintf(console, sizeof console, "%s%s", TILE_DIR, rel);
  platform_path(console, out, n);
}

static void read_elf_version(void) {
  char p[700], elf[700];
  real_path("/gamevault.version", p, sizeof p);
  real_path("/gamevault.elf", elf, sizeof elf);
  char *v = read_file(p, NULL);
  pthread_mutex_lock(&lock);
  snprintf(elf_version, sizeof elf_version, "%s", v && file_exists(elf) ? v : "");
  char *nl = strchr(elf_version, '\n');
  if (nl) *nl = 0;
  pthread_mutex_unlock(&lock);
  free(v);
}

/* Writes the tile's app folder (eboot.bin, sce_sys) where ShadowMountPlus finds it. */
static void install_folder(void) {
  char dir[700];
  real_path("/sce_sys", dir, sizeof dir);
  bool ok = mkdir_p(dir) == 0;
  int written = 0;
  for (const asset_t *a = tile_assets; ok && a->path; a++) {
    char p[700];
    real_path(a->path, p, sizeof p);
    size_t len = 0;
    char *have = read_file(p, &len);
    bool same = have && len == a->len && !memcmp(have, a->data, len);
    free(have);
    if (same) continue;
    if (write_file_atomic(p, (const char *)a->data, a->len)) ok = false;
    else written++;
  }
  pthread_mutex_lock(&lock);
  installed = ok;
  snprintf(install_error, sizeof install_error, "%s", ok ? "" : "Could not write the tile's folder in /data/homebrew");
  pthread_mutex_unlock(&lock);
  if (written) log_msg("home-screen tile: wrote %d file(s) to %s", written, TILE_DIR);
}

/* ---- newest version, from GameVault ------------------------------------------------ */

typedef struct {
  FILE *f;
  int64_t got;
} sink_t;

static int to_file(void *ctx, const char *data, size_t len) {
  sink_t *s = ctx;
  if (fwrite(data, 1, len, s->f) != len) return 1;
  s->got += (int64_t)len;
  return 0;
}

static void set_check_error(const char *msg) {
  pthread_mutex_lock(&lock);
  snprintf(check_error, sizeof check_error, "%s", msg);
  checked_at = now_ms();
  pthread_mutex_unlock(&lock);
}

static void check(void) {
  settings_t s = settings_get();
  if (!s.server[0]) return;
  char url[400], err[200] = { 0 };
  snprintf(url, sizeof url, "%s/api/v1/client", s.server);
  sbuf_t body;
  sbuf_init(&body);
  int status = 0;
  int rc = http_get_text(url, &body, &status, err, sizeof err);
  jval_t *j = rc ? NULL : json_parse(body.data);
  sbuf_free(&body);
  if (!j) {
    set_check_error(status == 404 ? "Your GameVault server is too old to offer updates; it updates itself overnight"
                                  : err[0] ? err : "Unexpected answer from GameVault");
    return;
  }
  const char *version = json_str(j, "version", "");
  bool available = json_bool(j, "available", false) && version[0];
  int64_t size = (int64_t)json_num(j, "size", 0);
  char latest[32];
  snprintf(latest, sizeof latest, "%s", available ? version : "");
  const char *server_error = json_str(j, "error", "");
  pthread_mutex_lock(&lock);
  snprintf(latest_version, sizeof latest_version, "%s", latest);
  snprintf(check_error, sizeof check_error, "%s", available ? "" : server_error[0] ? server_error : "GameVault has no copy of GameVault for PS5 yet");
  checked_at = now_ms();
  bool need = available && strcmp(elf_version, latest);
  pthread_mutex_unlock(&lock);
  json_free(j);
  if (!need) return;

  /* Keep the newest gamevault.elf next to the tile: the tile starts it, and updates use it. */
  char part[700], final[700], ver[700];
  real_path("/gamevault.elf.part", part, sizeof part);
  real_path("/gamevault.elf", final, sizeof final);
  real_path("/gamevault.version", ver, sizeof ver);
  sink_t sink = { fopen(part, "wb"), 0 };
  if (!sink.f) return;
  snprintf(url, sizeof url, "%s/api/v1/client/gamevault.elf", s.server);
  rc = http_get(url, -1, -1, to_file, &sink, NULL, NULL, err, sizeof err);
  fclose(sink.f);
  char head[4] = { 0 };
  FILE *h = fopen(part, "rb");
  if (h) { if (fread(head, 1, 4, h) != 4) head[0] = 0; fclose(h); }
  if (rc || (size && sink.got != size) || memcmp(head, "\x7f" "ELF", 4)) {
    unlink(part);
    char msg[300];
    snprintf(msg, sizeof msg, "Could not download version %s: %s", latest, rc ? err : "the file was not a valid ELF");
    set_check_error(msg);
    return;
  }
  if (rename(part, final)) { unlink(part); return; }
  write_file_atomic(ver, latest, strlen(latest));
  read_elf_version();
  log_msg("saved GameVault %s next to the home-screen tile", latest);
}

static void *tile_thread(void *arg) {
  (void)arg;
  install_folder();
  read_elf_version();
  char last_server[256] = { 0 };
  int64_t last = 0;
  sleep(3);
  for (;;) {
    settings_t s = settings_get();
    if (check_requested || strcmp(s.server, last_server) || now_ms() - last > 6LL * 3600 * 1000) {
      check_requested = 0;
      snprintf(last_server, sizeof last_server, "%s", s.server);
      check();
      last = now_ms();
    }
    /* Put the folder back if it was deleted. */
    char p[700];
    real_path("/sce_sys/param.json", p, sizeof p);
    if (!file_exists(p)) install_folder();
    sleep(5);
  }
  return NULL;
}

void tile_init(void) {
  pthread_t t;
  if (pthread_create(&t, NULL, tile_thread, NULL) == 0) pthread_detach(t);
}

void tile_check_now(void) { check_requested = 1; }

void tile_json(sbuf_t *out) {
  pthread_mutex_lock(&lock);
  sbuf_puts(out, "{");
  jw_kv_bool(out, "installed", installed, false);
  jw_kv_str(out, "path", TILE_DIR, true);
  jw_kv_str(out, "titleId", TILE_TITLE_ID, true);
  jw_kv_str(out, "error", install_error, true);
  jw_kv_str(out, "elfVersion", elf_version, true);
  jw_kv_str(out, "latestVersion", latest_version, true);
  jw_kv_bool(out, "updateAvailable", newer_than_running(latest_version), true);
  jw_kv_bool(out, "updateReady", newer_than_running(elf_version), true);
  jw_kv_str(out, "checkError", check_error, true);
  jw_kv_int(out, "checkedAt", checked_at, true);
  sbuf_puts(out, "}");
  pthread_mutex_unlock(&lock);
}

static void *open_thread(void *arg) {
  (void)arg;
  /* Let the tile app close first: wait (up to 6 s) until no app is in front, then a moment more. */
  for (int i = 0; i < 30 && platform_game_running(); i++) usleep(200000);
  usleep(700000);
  int rc = platform_open_browser("http://127.0.0.1:8687/?tv=1");
  log_msg("tile: opened the browser (result 0x%08x)", (unsigned)rc);
  if (rc) {
    sleep(2);
    rc = platform_open_browser("http://127.0.0.1:8687/?tv=1");
    log_msg("tile: opened the browser again (result 0x%08x)", (unsigned)rc);
  }
  return NULL;
}

void tile_open_later(void) {
  log_msg("tile: asked to open the interface");
  pthread_t t;
  if (pthread_create(&t, NULL, open_thread, NULL) == 0) pthread_detach(t);
}

/* ---- restarting with a newer version ------------------------------------------------- */

static int send_to_loader(const char *path, char *err, size_t errlen) {
  static const int ports[] = { 9021, 9027, 0 };
  size_t len = 0;
  char *elf = read_file(path, &len);
  if (!elf) { snprintf(err, errlen, "The new version isn't on the PS5 yet"); return -1; }
  int fd = -1;
  for (int i = 0; ports[i] && fd < 0; i++) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)ports[i]);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); fd = -1; }
  }
  if (fd < 0) {
    free(elf);
    snprintf(err, errlen, "No ELF loader is listening on port 9021 or 9027");
    return -1;
  }
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, elf + off, len - off, 0);
    if (n <= 0) break;
    off += (size_t)n;
  }
  close(fd);
  free(elf);
  if (off != len) { snprintf(err, errlen, "The ELF loader stopped reading"); return -1; }
  return 0;
}

int tile_update_now(char *err, size_t errlen) {
  pthread_mutex_lock(&lock);
  bool ready = newer_than_running(elf_version);
  char version[32];
  snprintf(version, sizeof version, "%s", elf_version);
  pthread_mutex_unlock(&lock);
  if (!ready) { snprintf(err, errlen, "There is no newer version on the PS5 to start"); return -1; }
  char elf[700];
  real_path("/gamevault.elf", elf, sizeof elf);
  if (!file_exists(elf)) { snprintf(err, errlen, "The new version isn't on the PS5 yet"); return -1; }

  log_msg("restarting as GameVault %s", version);
  downloads_prepare_restart();
  http_server_stop();      /* the new copy needs the port */
  usleep(300000);
  if (send_to_loader(elf, err, errlen)) {
    log_msg("update failed: %s", err);
    downloads_cancel_restart();
    return -2;
  }
  _exit(0);
}
