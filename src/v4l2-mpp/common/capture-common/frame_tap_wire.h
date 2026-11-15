#ifndef FRAME_TAP_WIRE_H
#define FRAME_TAP_WIRE_H

#include <stdbool.h>
#include <stdint.h>

/* What travels on the frame tap socket, from the tap inside the printer's video service to the
 * capture: this header, then frame_bytes of NV12 picture with no padding between the lines.
 * The capture asks for each picture by writing FRAME_TAP_FRAME_REQUEST, so the tap only ever
 * sends a picture someone is waiting for, and never has to drop half of one.
 *
 * Both ends are built from this one file, so the shape cannot drift between them. It needs no
 * kernel or Rockchip header, so the tests compile it on its own.
 */
#define FRAME_TAP_MAGIC 0x50415446u
#define FRAME_TAP_FRAME_REQUEST 'F'

typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t frame_bytes;
} frame_tap_header_t;

static inline uint32_t frame_tap_nv12_bytes(uint32_t width, uint32_t height)
{
    return width * height * 3 / 2;
}

/* A picture of a size the capture was not started at would be encoded with the wrong lines, so
 * it is refused rather than mislabelled. */
static inline bool frame_tap_header_is_valid(const frame_tap_header_t *header,
                                             uint32_t expected_width, uint32_t expected_height)
{
    if (header->magic != FRAME_TAP_MAGIC)
        return false;
    if (header->width != expected_width || header->height != expected_height)
        return false;
    return header->frame_bytes == frame_tap_nv12_bytes(expected_width, expected_height);
}

#endif /* FRAME_TAP_WIRE_H */
