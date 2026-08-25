#ifndef FRAME_BACKLOG_H
#define FRAME_BACKLOG_H

#include <stdbool.h>
#include <stddef.h>

/* Whether offering this frame to this reader would end in a picture cut in half.
 *
 * A frame larger than the socket can hold is handed over in pieces, and it only ever completes
 * because the reader keeps taking bytes out of the way. Offering one to a reader that is still
 * behind on the last frame ends at the write timeout, in the middle of the picture, with the
 * reader holding the top half of it. Such a reader is caught up first and loses the frame whole.
 *
 * A frame that does fit is allowed the socket's own room, which is what a browser watching the
 * MJPEG stream leans on: it falls a little behind between frames and catches up again, and only a
 * whole frame still outstanding means it is genuinely too slow to keep up.
 */
static inline bool reader_cannot_take_whole_frame(size_t unsent_bytes,
                                                  size_t socket_capacity_bytes,
                                                  size_t offered_frame_bytes)
{
    if (offered_frame_bytes > socket_capacity_bytes)
        return unsent_bytes > 0;
    return unsent_bytes >= offered_frame_bytes;
}

#endif /* FRAME_BACKLOG_H */
