#ifndef STRIDED_NV12_H
#define STRIDED_NV12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A picture as the camera pipeline hands it over: NV12, but each line padded out to stride_width
 * bytes and the brightness plane padded out to stride_height lines, all inside one memory block
 * of block_bytes. The capture wants the lines packed with no padding, so they are copied across.
 *
 * It needs no Rockchip header, so the tests compile it on its own.
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride_width;
    uint32_t stride_height;
    uint64_t block_bytes;
} strided_nv12_t;

/* The colour plane starts after the padded brightness plane and holds one line per two picture
 * lines, so this is how far into the block the last colour byte reaches. */
static inline uint64_t strided_nv12_reach_bytes(const strided_nv12_t *picture)
{
    uint64_t brightness_plane_bytes = (uint64_t)picture->stride_width * picture->stride_height;
    return brightness_plane_bytes + (uint64_t)picture->stride_width * (picture->height / 2);
}

/* Whether copying the picture out stays inside its block. A pipeline that ever handed over a
 * different layout would otherwise have the copy read past the end of the block. */
static inline bool strided_nv12_is_packable(const strided_nv12_t *picture)
{
    if (picture->width == 0 || picture->height == 0)
        return false;
    if (picture->width % 2 != 0 || picture->height % 2 != 0)
        return false;
    if (picture->width > picture->stride_width || picture->height > picture->stride_height)
        return false;
    return strided_nv12_reach_bytes(picture) <= picture->block_bytes;
}

static inline void strided_plane_pack(const uint8_t *padded_plane, size_t stride_width,
                                      uint8_t *packed_plane, size_t line_bytes, size_t line_count)
{
    size_t line;

    for (line = 0; line < line_count; line++)
        memcpy(packed_plane + line * line_bytes, padded_plane + line * stride_width, line_bytes);
}

/* packed_picture must hold width * height * 3 / 2 bytes. */
static inline void strided_nv12_pack(const strided_nv12_t *picture, const uint8_t *block,
                                     uint8_t *packed_picture)
{
    size_t brightness_plane_bytes = (size_t)picture->width * picture->height;
    const uint8_t *padded_colour_plane = block + (size_t)picture->stride_width * picture->stride_height;

    strided_plane_pack(block, picture->stride_width, packed_picture, picture->width, picture->height);
    strided_plane_pack(padded_colour_plane, picture->stride_width,
                       packed_picture + brightness_plane_bytes, picture->width, picture->height / 2);
}

#endif /* STRIDED_NV12_H */
