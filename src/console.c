#include "console.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "json.h"
#include "platform.h"
#include "tile.h"

#define MAX_INSTALLED 2048

static installed_t *items;
static int item_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const char *IMAGE_EXT[] = { ".exfat", ".ffpkg", ".ffpfs", ".ffpfsc", NULL };

/* ---- drives ---------------------------------------------------------------- */

static void fill_drive(drive_t *d, const char *id) {
  memset(d, 0, sizeof *d);
  snprintf(d->id, sizeof d->id, "%s", id);
  if (!strcmp(id, "internal")) {
    snprintf(d->label, sizeof d->label, "Internal SSD");
    snprintf(d->root, sizeof d->root, "/user");
    snprintf(d->homebrew, sizeof d->homebrew, "/data/homebrew");
  } else if (!strncmp(id, "usb", 3)) {
    snprintf(d->label, sizeof d->label, "USB drive %d", id[3] - '0' + 1);
    snprintf(d->root, sizeof d->root, "/mnt/%s", id);
    snprintf(d->homebrew, sizeof d->homebrew, "/mnt/%s/homebrew", id);
  } else {
    snprintf(d->label, sizeof d->label, "M.2 / extended storage %d", id[3] - '0' + 1);
    snprintf(d->root, sizeof d->root, "/mnt/%s", id);
    snprintf(d->homebrew, sizeof d->homebrew, "/mnt/%s/homebrew", id);
  }
  char real[600];
  platform_path(d->root, real, sizeof real);
  struct statvfs st;
  if (platform_is_mounted(d->root) && statvfs(real, &st) == 0 && st.f_blocks > 0) {
    d->present = true;
    d->free_bytes = (int64_t)st.f_bavail * (int64_t)st.f_frsize;
    d->total_bytes = (int64_t)st.f_blocks * (int64_t)st.f_frsize;
  } else if (!strcmp(id, "internal")) {
    /* Fall back to /data when /user can't be read. */
    platform_path("/data", real, sizeof real);
    if (statvfs(real, &st) == 0 && st.f_blocks > 0) {
      d->present = true;
      d->free_bytes = (int64_t)st.f_bavail * (int64_t)st.f_frsize;
      d->total_bytes = (int64_t)st.f_blocks * (int64_t)st.f_frsize;
    }
  }
}

int console_drives(drive_t out[MAX_DRIVES]) {
  int n = 0;
  fill_drive(&out[n++], "internal");
  char id[8];
  for (int i = 0; i < 8; i++) { snprintf(id, sizeof id, "usb%d", i); fill_drive(&out[n++], id); }
  for (int i = 0; i < 4; i++) { snprintf(id, sizeof id, "ext%d", i); fill_drive(&out[n++], id); }
  return n;
}

bool console_drive(const char *id, drive_t *out) {
  if (strcmp(id, "internal") && strncmp(id, "usb", 3) && strncmp(id, "ext", 3)) return false;
  fill_drive(out, id);
  return true;
}

void console_drives_json(sbuf_t *out) {
  drive_t d[MAX_DRIVES];
  int n = console_drives(d);
  sbuf_puts(out, "[");
  bool first = true;
  for (int i = 0; i < n; i++) {
    if (!d[i].present) continue;
    if (!first) sbuf_puts(out, ",");
    first = false;
    sbuf_puts(out, "{");
    jw_kv_str(out, "id", d[i].id, false);
    jw_kv_str(out, "label", d[i].label, true);
    jw_kv_str(out, "homebrew", d[i].homebrew, true);
    jw_kv_int(out, "free", d[i].free_bytes, true);
    jw_kv_int(out, "total", d[i].total_bytes, true);
    sbuf_puts(out, "}");
  }
  sbuf_puts(out, "]");
}

/* ---- param.sfo (PS4) ---------------------------------------------------------- */

static void sfo_value(const char *buf, size_t len, const char *want, char *out, size_t outlen) {
  out[0] = 0;
  if (len < 20 || memcmp(buf, "\0PSF", 4)) return;
  uint32_t keys = *(const uint32_t *)(buf + 8);
  uint32_t data = *(const uint32_t *)(buf + 12);
  uint32_t count = *(const uint32_t *)(buf + 16);
  for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= len; i++) {
    const char *e = buf + 20 + i * 16;
    uint16_t koff = *(const uint16_t *)e;
    uint16_t fmt = *(const uint16_t *)(e + 2);
    uint32_t dlen = *(const uint32_t *)(e + 4);
    uint32_t doff = *(const uint32_t *)(e + 12);
    if (keys + koff >= len || data + doff + dlen > len) continue;
    if (strcmp(buf + keys + koff, want)) continue;
    if (fmt == 0x0404) return;
    size_t n = dlen < outlen ? dlen : outlen - 1;
    memcpy(out, buf + data + doff, n);
    out[n] = 0;
    return;
  }
}

/* ---- scanning ------------------------------------------------------------------- */

static int64_t du(const char *real) {
  struct stat st;
  if (lstat(real, &st)) return 0;
  if (!S_ISDIR(st.st_mode)) return (int64_t)st.st_size;
  int64_t total = 0;
  DIR *d = opendir(real);
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[1100];
    snprintf(child, sizeof child, "%s/%s", real, e->d_name);
    total += du(child);
  }
  closedir(d);
  return total;
}

/* "PPSA01417-Title (v01.015.001).exfat" -> title id + version */
static void parse_image_name(const char *name, installed_t *it) {
  if ((!strncasecmp(name, "PPSA", 4) || !strncasecmp(name, "CUSA", 4)) && strlen(name) >= 9) {
    bool digits = true;
    for (int i = 4; i < 9; i++) if (!isdigit((unsigned char)name[i])) digits = false;
    if (digits) {
      for (int i = 0; i < 9; i++) it->title_id[i] = (char)toupper((unsigned char)name[i]);
      it->title_id[9] = 0;
    }
  }
  const char *v = strstr(name, "(v");
  if (v) {
    const char *end = strchr(v, ')');
    if (end && end - v - 2 < (long)sizeof it->version) {
      memcpy(it->version, v + 2, (size_t)(end - v - 2));
      it->version[end - v - 2] = 0;
    }
  }
  /* Title: text after "ID-" up to " (v" or the extension. */
  const char *t = it->title_id[0] && name[9] == '-' ? name + 10 : name;
  size_t tl = v ? (size_t)(v - t) : strlen(t);
  const char *dot = strrchr(t, '.');
  if (!v && dot) tl = (size_t)(dot - t);
  while (tl && t[tl - 1] == ' ') tl--;
  if (tl >= sizeof it->title) tl = sizeof it->title - 1;
  memcpy(it->title, t, tl);
  it->title[tl] = 0;
}

static bool is_image(const char *name) {
  size_t nl = strlen(name);
  for (int i = 0; IMAGE_EXT[i]; i++) {
    size_t el = strlen(IMAGE_EXT[i]);
    if (nl > el && !strcasecmp(name + nl - el, IMAGE_EXT[i])) return true;
  }
  return false;
}

static void add(installed_t *list, int *n, const installed_t *it) {
  if (*n < MAX_INSTALLED) list[(*n)++] = *it;
}

static void scan_dir(installed_t *list, int *n, const char *console_dir, const char *drive, int depth) {
  char real[600];
  platform_path(console_dir, real, sizeof real);
  DIR *d = opendir(real);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
    char cpath[600], rpath[1200];
    snprintf(cpath, sizeof cpath, "%s/%s", console_dir, e->d_name);
    snprintf(rpath, sizeof rpath, "%s/%s", real, e->d_name);
    struct stat st;
    if (stat(rpath, &st)) continue;
    installed_t it;
    memset(&it, 0, sizeof it);
    snprintf(it.path, sizeof it.path, "%s", cpath);
    snprintf(it.drive, sizeof it.drive, "%s", drive);
    it.mtime_ms = (int64_t)st.st_mtime * 1000;
    if (S_ISREG(st.st_mode) && is_image(e->d_name)) {
      snprintf(it.kind, sizeof it.kind, "image");
      it.size = (int64_t)st.st_size;
      parse_image_name(e->d_name, &it);
      if (it.title_id[0]) add(list, n, &it);
    } else if (S_ISDIR(st.st_mode)) {
      char param[1300];
      snprintf(param, sizeof param, "%s/sce_sys/param.json", rpath);
      char *text = read_file(param, NULL);
      if (text) {
        jval_t *j = json_parse(text);
        free(text);
        const char *tid = json_str(j, "titleId", NULL);
        if (tid && !strcmp(tid, TILE_TITLE_ID)) tid = NULL;  /* our own home-screen tile */
        if (tid) {
          snprintf(it.kind, sizeof it.kind, "folder");
          snprintf(it.title_id, sizeof it.title_id, "%s", tid);
          snprintf(it.version, sizeof it.version, "%s", json_str(j, "contentVersion", ""));
          jval_t *lp = json_get(j, "localizedParameters");
          jval_t *en = json_get(lp, "en-US");
          if (!en) en = json_get(lp, "en-GB");
          snprintf(it.title, sizeof it.title, "%s", json_str(en, "titleName", e->d_name));
          it.size = du(rpath);
          add(list, n, &it);
        }
        json_free(j);
      } else if (depth < 2) {
        scan_dir(list, n, cpath, drive, depth + 1);
      }
    }
  }
  closedir(d);
}

static void scan_ps4_apps(installed_t *list, int *n, const char *user_root, const char *drive) {
  char apps[300], real[700];
  snprintf(apps, sizeof apps, "%s/app", user_root);
  platform_path(apps, real, sizeof real);
  DIR *d = opendir(real);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "CUSA", 4) || strlen(e->d_name) != 9) continue;
    installed_t it;
    memset(&it, 0, sizeof it);
    snprintf(it.kind, sizeof it.kind, "ps4-app");
    snprintf(it.title_id, sizeof it.title_id, "%s", e->d_name);
    snprintf(it.drive, sizeof it.drive, "%s", drive);
    snprintf(it.path, sizeof it.path, "%s/%s", apps, e->d_name);
    char p[900];
    snprintf(p, sizeof p, "%s/%s", real, e->d_name);
    it.size = du(p);
    char meta[300], rmeta[800];
    snprintf(meta, sizeof meta, "/system_data/priv/appmeta/%s/param.sfo", e->d_name);
    platform_path(meta, rmeta, sizeof rmeta);
    size_t len = 0;
    char *sfo = read_file(rmeta, &len);
    if (sfo) {
      sfo_value(sfo, len, "APP_VER", it.version, sizeof it.version);
      sfo_value(sfo, len, "TITLE", it.title, sizeof it.title);
      free(sfo);
    }
    if (!it.title[0]) snprintf(it.title, sizeof it.title, "%s", e->d_name);
    add(list, n, &it);
  }
  closedir(d);
}

void console_rescan(void) {
  installed_t *list = calloc(MAX_INSTALLED, sizeof *list);
  if (!list) return;
  int n = 0;
  drive_t drives[MAX_DRIVES];
  int dn = console_drives(drives);
  for (int i = 0; i < dn; i++) {
    if (!drives[i].present && strcmp(drives[i].id, "internal")) continue;
    scan_dir(list, &n, drives[i].homebrew, drives[i].id, 0);
    char eta[128];
    snprintf(eta, sizeof eta, "%s/etaHEN/games", !strcmp(drives[i].id, "internal") ? "/data" : drives[i].root);
    scan_dir(list, &n, eta, drives[i].id, 0);
  }
  scan_ps4_apps(list, &n, "/user", "internal");
  for (int i = 0; i < 4; i++) {
    char r[32];
    snprintf(r, sizeof r, "/mnt/ext%d/user", i);
    char id[8];
    snprintf(id, sizeof id, "ext%d", i);
    scan_ps4_apps(list, &n, r, id);
  }
  pthread_mutex_lock(&lock);
  free(items);
  items = list;
  item_count = n;
  pthread_mutex_unlock(&lock);
}

static void *rescan_thread(void *arg) {
  (void)arg;
  for (;;) {
    sleep(30);
    console_rescan();
  }
  return NULL;
}

void console_init(void) {
  console_rescan();
  pthread_t t;
  if (pthread_create(&t, NULL, rescan_thread, NULL) == 0) pthread_detach(t);
}

void console_installed_json(sbuf_t *out) {
  pthread_mutex_lock(&lock);
  sbuf_puts(out, "[");
  for (int i = 0; i < item_count; i++) {
    installed_t *it = &items[i];
    if (i) sbuf_puts(out, ",");
    sbuf_puts(out, "{");
    jw_kv_str(out, "titleId", it->title_id, false);
    jw_kv_str(out, "version", it->version, true);
    jw_kv_str(out, "title", it->title, true);
    jw_kv_str(out, "path", it->path, true);
    jw_kv_str(out, "drive", it->drive, true);
    jw_kv_str(out, "kind", it->kind, true);
    jw_kv_int(out, "size", it->size, true);
    jw_kv_int(out, "modified", it->mtime_ms, true);
    sbuf_puts(out, "}");
  }
  sbuf_puts(out, "]");
  pthread_mutex_unlock(&lock);
}

bool console_has(const char *title_id, char *version_out, size_t vlen) {
  bool found = false;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < item_count; i++) {
    if (!strcasecmp(items[i].title_id, title_id)) {
      if (version_out) snprintf(version_out, vlen, "%s", items[i].version);
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  return found;
}

int console_delete(const char *path, char *err, size_t errlen) {
  installed_t it;
  bool found = false;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < item_count; i++) {
    if (!strcmp(items[i].path, path)) { it = items[i]; found = true; break; }
  }
  pthread_mutex_unlock(&lock);
  if (!found) {
    snprintf(err, errlen, "That game is not in the list of games on this PS5");
    return -1;
  }
  if (!strcmp(it.kind, "ps4-app")) {
    snprintf(err, errlen, "Delete installed PS4 games from the PS5's own Storage settings");
    return -1;
  }
  char real[700];
  platform_path(it.path, real, sizeof real);
  if (rm_rf(real)) {
    snprintf(err, errlen, "Could not delete %s", it.path);
    return -1;
  }
  console_rescan();
  return 0;
}
