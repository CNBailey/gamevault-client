// GameVault for PS5 - Decoding covers (JPEG and PNG) for the native app.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gv
{

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, top row first
};

// Decodes a JPEG or PNG; false when it can't.
bool decode_image(const std::string &bytes, Image *image);
// Halves the picture (box filter) until neither side is over max_side.
void shrink_to(Image *image, int max_side);
// Sends a picture to the GPU; UI (GL) thread only. Returns the texture name.
std::uint32_t upload_texture(const Image &image);
void delete_texture(std::uint32_t texture);

} // namespace gv
