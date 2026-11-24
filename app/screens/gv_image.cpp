// GameVault for PS5 - Decoding covers (JPEG and PNG) for the native app.
// Copyright (C) 2026 GameVault contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/gv_image.hpp"

#include "third_party/stb_image/stb_image.h"

#include <GL/glcorearb.h>

namespace gv
{

bool decode_image(const std::string &bytes, Image *image)
{
    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                                                  static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels)
        return false;
    image->width = w;
    image->height = h;
    image->rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(pixels);
    return true;
}

void shrink_to(Image *image, int max_side)
{
    while (image->width > max_side || image->height > max_side)
    {
        const int w = image->width / 2, h = image->height / 2;
        if (w < 1 || h < 1)
            return;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        const int stride = image->width * 4;
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const std::uint8_t *a = &image->rgba[static_cast<std::size_t>(2 * y) * stride + static_cast<std::size_t>(2 * x) * 4];
                const std::uint8_t *b = a + 4;
                const std::uint8_t *c = a + stride;
                const std::uint8_t *d = c + 4;
                std::uint8_t *o = &out[(static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4];
                for (int k = 0; k < 4; ++k)
                    o[k] = static_cast<std::uint8_t>((a[k] + b[k] + c[k] + d[k] + 2) / 4);
            }
        }
        image->width = w;
        image->height = h;
        image->rgba.swap(out);
    }
}

std::uint32_t upload_texture(const Image &image)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 image.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

void delete_texture(std::uint32_t texture)
{
    if (texture)
    {
        const GLuint t = texture;
        glDeleteTextures(1, &t);
    }
}

} // namespace gv
