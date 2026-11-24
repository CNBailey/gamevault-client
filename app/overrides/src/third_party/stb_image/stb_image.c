/* GameVault for PS5 - the one place stb_image's code is compiled.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * stb_image.h (public domain or MIT, see LICENSE beside it) is copied here by
 * tools/build-app.sh from app/third_party/stb_image. Covers are JPEG (from
 * IGDB, cached by GameVault) or PNG (the game's own icon0.png).
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 4096
#include "third_party/stb_image/stb_image.h"
