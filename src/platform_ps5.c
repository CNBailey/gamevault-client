/* PS5 implementation of platform.h (built with the ps5-payload-sdk). */
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <ps5/kernel.h>

/* --- System prototypes (libkernel, SceSystemService, SceUserService, SceAppInstUtil) --- */
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

int sceUserServiceInitialize(void *);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *);
int sceSystemServiceGetAppIdOfRunningBigApp(void);

typedef struct pkg_metadata {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} pkg_metadata_t;

typedef struct pkg_info {
  char content_id[48];
  int type;
  int platform;
} pkg_info_t;

typedef struct playgo_info {
  char lang[8][30];
  char scenario_ids[3][64];
  char content_ids[64];
  long unknown[810];
} playgo_info_t;

int sceAppInstUtilInitialize(void);
int sceAppInstUtilInstallByPackage(const pkg_metadata_t *, pkg_info_t *, playgo_info_t *);

static bool appinst_ready = false;

void platform_init(void) {
  sceUserServiceInitialize(0);
  appinst_ready = sceAppInstUtilInitialize() == 0;
}

const char *platform_data_dir(void) { return "/data/gamevault"; }

void platform_path(const char *console_path, char *out, size_t outlen) {
  snprintf(out, outlen, "%s", console_path);
}

void platform_firmware(char *out, size_t outlen) {
  uint32_t fw = kernel_get_fw_version();
  /* 0xMMmm.... -> "M.mm" (BCD) */
  unsigned major = (fw >> 24) & 0xFF;
  unsigned minor = (fw >> 16) & 0xFF;
  snprintf(out, outlen, "%x.%02x", major, minor);
}

bool platform_game_running(void) {
  return sceSystemServiceGetAppIdOfRunningBigApp() > 0;
}

void platform_notify(const char *title, const char *body) {
  notify_request_t req;
  bzero(&req, sizeof req);
  snprintf(req.message, sizeof req.message, "%s\n%s", title, body ? body : "");
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

int platform_open_browser(const char *url) {
  return sceSystemServiceLaunchWebBrowser(url, 0);
}

int platform_install_pkg(const char *url, const char *content_id, const char *name, const char *icon_url, char *err, size_t errlen) {
  if (!appinst_ready && sceAppInstUtilInitialize() != 0) {
    snprintf(err, errlen, "The console's installer is not available");
    return -1;
  }
  appinst_ready = true;
  playgo_info_t playgo;
  pkg_info_t info;
  memset(&playgo, 0, sizeof playgo);
  memset(&info, 0, sizeof info);
  pkg_metadata_t meta = {
    .uri = url,
    .ex_uri = "",
    .playgo_scenario_id = "",
    .content_id = content_id ? content_id : "",
    .content_name = name ? name : "",
    .icon_url = icon_url ? icon_url : ""
  };
  int rc = sceAppInstUtilInstallByPackage(&meta, &info, &playgo);
  if (rc) snprintf(err, errlen, "The console's installer refused the package (error 0x%08x)", (unsigned)rc);
  return rc;
}
