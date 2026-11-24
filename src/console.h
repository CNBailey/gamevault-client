/* What is on this PS5: drives, free space, games already there. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "util.h"

typedef struct {
  char id[16];          /* internal, usb0..usb7, ext0..ext3 */
  char label[48];
  char root[64];        /* console path of the drive */
  char homebrew[96];    /* where ShadowMountPlus looks for games on this drive */
  bool present;
  int64_t free_bytes;
  int64_t total_bytes;
} drive_t;

#define MAX_DRIVES 13

typedef struct {
  char title_id[16];
  char version[32];
  char title[256];
  char path[512];       /* console path */
  char drive[16];
  char kind[16];        /* image | folder | ps4-app */
  int64_t size;
  int64_t mtime_ms;
} installed_t;

void console_init(void);
int console_drives(drive_t out[MAX_DRIVES]);              /* all known drives (present or not) */
bool console_drive(const char *id, drive_t *out);
void console_rescan(void);                                /* refresh the installed list now */
void console_installed_json(sbuf_t *out);                 /* JSON array */
void console_drives_json(sbuf_t *out);                    /* JSON array */
bool console_has(const char *title_id, char *version_out, size_t vlen);
/* Deletes a game found by the last scan, by its console path. */
int console_delete(const char *path, char *err, size_t errlen);
