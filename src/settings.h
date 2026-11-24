#pragma once

#include <stdbool.h>
#include "util.h"

typedef struct {
  char server[256];      /* GameVault, e.g. http://192.168.0.32:8686 */
  char ps5_drive[16];    /* "internal", "usb0".."usb7", "ext0".."ext3" */
  int connections;       /* parallel connections per download, 1..8; 0 = automatic */
  int measured_connections; /* best count from the last speed test (used by automatic) */
  int measured_mbps;     /* speed reached in that test, MB/s */
  int play_limit_mbps;   /* MB/s cap while a game is running, 0 = no cap */
  bool notify;           /* pop-up when a download finishes or fails */
  bool check_files;      /* check size and image header after download */
  char transfer[8];      /* "auto" | "http" | "nfs" */
  char nfs_export[256];  /* e.g. synology:/volume1/Games (NFS, when supported) */
} settings_t;

void settings_load(void);
void settings_save(void);
settings_t settings_get(void);                      /* copy, thread-safe */
void settings_set(const settings_t *s);
void settings_to_json(const settings_t *s, sbuf_t *out);
/* Applies the fields present in a JSON object; returns an error message or NULL. */
const char *settings_apply_json(settings_t *s, const char *json);
