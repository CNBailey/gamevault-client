/*
 * Linux/macOS implementation of platform.h, for development and automated tests.
 * GV_ROOT   : directory that stands in for the console's root (default ./gvroot)
 * GV_DATA   : data directory (default $GV_ROOT/data/gamevault)
 * GV_FW     : firmware to report (default 9.60)
 * GV_GAME_RUNNING=1 : pretend a game is running
 */
#include "platform.h"
#include "util.h"

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

static char root[512];
static char data_dir[600];

void platform_init(void) {
  const char *r = getenv("GV_ROOT");
  snprintf(root, sizeof root, "%s", r && *r ? r : "./gvroot");
  const char *d = getenv("GV_DATA");
  if (d && *d) snprintf(data_dir, sizeof data_dir, "%s", d);
  else snprintf(data_dir, sizeof data_dir, "%s/data/gamevault", root);
}

const char *platform_data_dir(void) { return data_dir; }

void platform_path(const char *console_path, char *out, size_t outlen) {
  snprintf(out, outlen, "%s%s", root, console_path);
}

void platform_firmware(char *out, size_t outlen) {
  const char *fw = getenv("GV_FW");
  snprintf(out, outlen, "%s", fw && *fw ? fw : "9.60");
}

bool platform_game_running(void) {
  const char *g = getenv("GV_GAME_RUNNING");
  return g && *g == '1';
}

void platform_notify(const char *title, const char *body) {
  log_msg("NOTIFY: %s - %s", title, body ? body : "");
}

int platform_open_browser(const char *url) {
  log_msg("Open the TV interface at %s", url);
  return 0;
}

int platform_install_pkg(const char *url, const char *content_id, const char *name, const char *icon_url, char *err, size_t errlen) {
  (void)icon_url;
  log_msg("INSTALL PKG %s (%s, %s)", url, content_id ? content_id : "", name ? name : "");
  const char *fail = getenv("GV_INSTALL_FAIL");
  if (fail && *fail == '1') {
    snprintf(err, errlen, "Simulated installer failure");
    return -1;
  }
  /* Record the "installed" app like the console would, so scans pick it up. */
  if (content_id && strlen(content_id) >= 16) {
    char tid[10];
    memcpy(tid, content_id + 7, 9);
    tid[9] = 0;
    char p[700];
    snprintf(p, sizeof p, "%s/user/app/%s", root, tid);
    mkdir_p(p);
  }
  return 0;
}

bool platform_is_mounted(const char *console_path) {
  char real[600], parent[700];
  platform_path(console_path, real, sizeof real);
  if (!strcmp(console_path, "/user") || !strcmp(console_path, "/data")) return dir_exists(real);
  /* For tests: a ".mounted" file stands in for a real mount. */
  snprintf(parent, sizeof parent, "%s/.mounted", real);
  if (file_exists(parent)) return true;
  struct stat a, b;
  snprintf(parent, sizeof parent, "%s/..", real);
  return stat(real, &a) == 0 && stat(parent, &b) == 0 && a.st_dev != b.st_dev;
}
