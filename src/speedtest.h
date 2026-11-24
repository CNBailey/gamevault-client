#pragma once

#include <stdint.h>
#include "util.h"

/* Measures download speed from GameVault with several connection counts (about 16 s). */
int speedtest_run(int64_t game_id, sbuf_t *out, char *err, size_t errlen);
