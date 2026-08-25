#ifndef FRAME_GEOMETRY_H
#define FRAME_GEOMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The pixel formats the shim can present. They are named here, rather than taken from the kernel
   headers, so this file compiles on its own and its arithmetic can be tested without a printer. */
#define FRAME_FOURCC(first, second, third, fourth)                          \
    ((uint32_t)(first) | ((uint32_t)(second) << 8) |                        \
     ((uint32_t)(third) << 16) | ((uint32_t)(fourth) << 24))

#define FRAME_FORMAT_NV12  FRAME_FOURCC('N', 'V', '1', '2')
#define FRAME_FORMAT_YUYV  FRAME_FOURCC('Y', 'U', 'Y', 'V')
#define FRAME_FORMAT_MJPEG FRAME_FOURCC('M', 'J', 'P', 'G')
#define FRAME_FORMAT_JPEG  FRAME_FOURCC('J', 'P', 'E', 'G')

/* How many bytes one whole picture takes, or 0 when the format has no fixed length. NV12 keeps a
   full brightness plane and a half sized colour plane, so it is one and a half bytes per pixel, not
   two. A compressed picture is whatever the encoder made of it, so it ends when the sender hangs
   up and no count describes it. */
static inline size_t whole_frame_bytes(uint32_t pixel_format, unsigned int width, unsigned int height)
{
    if (pixel_format == FRAME_FORMAT_NV12)
        return (size_t)width * height * 3 / 2;
    if (pixel_format == FRAME_FORMAT_YUYV)
        return (size_t)width * height * 2;
    return 0;
}

/* The bytes one row of the picture occupies. A compressed picture has no rows to measure. */
static inline size_t frame_bytes_per_line(uint32_t pixel_format, unsigned int width)
{
    if (pixel_format == FRAME_FORMAT_NV12)
        return width;
    if (pixel_format == FRAME_FORMAT_YUYV)
        return (size_t)width * 2;
    return 0;
}

/* The buffer the video stack is handed. A fixed length format is given exactly one picture, so a
   read can never run past the end of this frame and into the start of the next one. A compressed
   format is given room for the worst case its encoder could produce. */
static inline size_t capture_buffer_bytes(uint32_t pixel_format, unsigned int width, unsigned int height)
{
    size_t fixed_length = whole_frame_bytes(pixel_format, width, height);
    if (fixed_length > 0)
        return fixed_length;
    return (size_t)width * height * 2;
}

/* Whether what arrived is a whole picture. A fixed length format is whole only at its exact size:
   anything shorter is a reader that was cut off, and handing that on is what puts a half drawn
   picture in the timelapse. */
static inline bool frame_read_is_whole(size_t expected_frame_bytes, size_t bytes_read)
{
    if (expected_frame_bytes == 0)
        return bytes_read > 0;
    return bytes_read == expected_frame_bytes;
}

#endif /* FRAME_GEOMETRY_H */
