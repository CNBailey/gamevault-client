/*
 * GameVault for PS5 - a background payload that downloads games from a
 * GameVault server to the console and serves its own interface on port 8687.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api.h"
#include "console.h"
#include "downloads.h"
#include "http_server.h"
#include "platform.h"
#include "settings.h"

#ifndef GV_VERSION
#define GV_VERSION "dev"
#endif

#define PORT 8687

int main(void) {
  platform_init();
  log_msg("GameVault for PS5 %s starting", GV_VERSION);

  const char *port_env = getenv("GV_PORT");
  int port = port_env && *port_env ? atoi(port_env) : PORT;
  char url[64];
  snprintf(url, sizeof url, "http://127.0.0.1:%d/?tv=1", port);

  mkdir_p(platform_data_dir());
  settings_load();
  console_init();

  if (http_server_start(port, api_handle)) {
    /* Already running (loaded twice): just bring the interface up again. */
    log_msg("port %d is in use; GameVault is probably already running", port);
    platform_open_browser(url);
    return 0;
  }
  downloads_init();
  log_msg("listening on port %d", port);
  platform_notify("GameVault", "Running. Open it from any browser on port 8687.");
  const char *no_browser = getenv("GV_NO_BROWSER");
  if (!no_browser || *no_browser != '1') platform_open_browser(url);

  for (;;) sleep(3600);
  return 0;
}
