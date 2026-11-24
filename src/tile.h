/* The GameVault tile on the PS5 home screen, and updating GameVault itself. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "util.h"

#define TILE_TITLE_ID "PPSA99687"
#define TILE_DIR "/data/homebrew/" TILE_TITLE_ID

/* Puts the tile's app folder in place and starts the update check. */
void tile_init(void);
/* {installed, path, elfVersion, latestVersion, updateAvailable, updateReady, checkError, checkedAt} */
void tile_json(sbuf_t *out);
/* Opens the interface in the PS5 browser after a moment (the tile app closes first). */
void tile_open_later(void);
/* Checks GameVault for a newer version now. */
void tile_check_now(void);
/* Restarts GameVault with the newer version. Returns only on failure:
   -1 nothing was stopped, -2 the web server was stopped and must be restarted. */
int tile_update_now(char *err, size_t errlen);
