/* The web interface, embedded in the payload by tools/embed.py. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char *path;   /* "/index.html" */
  const char *mime;
  const unsigned char *data;
  size_t len;
  bool gzip;          /* data is gzip-compressed */
} asset_t;

const asset_t *asset_find(const char *path);
