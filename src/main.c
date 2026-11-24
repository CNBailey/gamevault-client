/*
 * GameVault for PS5 - a background payload that downloads games from a
 * GameVault server to the console and serves its own interface on port 8687.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "api.h"
#include "console.h"
#include "downloads.h"
#include "http_server.h"
#include "platform.h"
#include "settings.h"
#include "tile.h"

#ifndef GV_VERSION
#define GV_VERSION "dev"
#endif

#define PORT 8687

/* The PS5's address on the network: the source address a UDP socket would use
   to reach the GameVault server (connecting a UDP socket sends nothing). */
static void local_ip(char *out, size_t n) {
  snprintf(out, n, "<PS5 address>");
  settings_t s = settings_get();
  url_t u;
  struct sockaddr_in to;
  memset(&to, 0, sizeof to);
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  if (!s.server[0] || !url_parse(s.server, &u) || inet_pton(AF_INET, u.host, &to.sin_addr) != 1)
    inet_pton(AF_INET, "192.168.0.1", &to.sin_addr);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  struct sockaddr_in me;
  socklen_t len = sizeof me;
  if (connect(fd, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(fd, (struct sockaddr *)&me, &len) == 0)
    inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)n);
  close(fd);
}

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

  char ip[64];
  local_ip(ip, sizeof ip);
  char where[160];
  snprintf(where, sizeof where, "Open http://%s:%d in a browser, or press the GameVault tile.", ip, port);

  if (http_server_start(port, api_handle)) {
    /* Already running (loaded twice). */
    log_msg("port %d is in use; GameVault is probably already running", port);
    platform_notify("GameVault is already running", where);
    return 0;
  }
  api_set_port(port);
  downloads_init();
  tile_init();
  log_msg("listening on port %d", port);
  /* Stay in the background: the tile or any browser opens the interface. */
  platform_notify("GameVault is running", where);
  log_msg("%s", where);
  (void)url;

  for (;;) sleep(3600);
  return 0;
}
