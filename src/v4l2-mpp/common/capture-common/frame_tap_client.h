#ifndef FRAME_TAP_CLIENT_H
#define FRAME_TAP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "frame_tap_wire.h"
#include "log.h"

/* The capture's end of the frame tap. On firmware where the printer's own video service keeps the
 * camera, the capture cannot open the device; it asks the tap inside that service for each picture
 * instead, one request, one whole picture back.
 *
 * The service starts and stops on its own schedule, and the capture must outlive that: a capture
 * that exited would be restarted only a few times before it gave up for good. So every failure
 * here hangs up, waits a moment and reports "no frame", and the next call tries again.
 */
#define FRAME_TAP_READ_TIMEOUT_S 2
#define FRAME_TAP_RETRY_PAUSE_US 1000000

typedef struct {
    const char *socket_path;
    int socket_fd;
    uint32_t width;
    uint32_t height;
    uint8_t *picture;
    size_t picture_bytes;
    bool in_trouble;
} frame_tap_client_t;

static int frame_tap_client_open(frame_tap_client_t *client, const char *socket_path,
                                 uint32_t width, uint32_t height)
{
    client->socket_path = socket_path;
    client->socket_fd = -1;
    client->width = width;
    client->height = height;
    client->picture_bytes = frame_tap_nv12_bytes(width, height);
    client->picture = malloc(client->picture_bytes);
    client->in_trouble = false;
    return client->picture ? 0 : -1;
}

static void frame_tap_client_hang_up(frame_tap_client_t *client)
{
    if (client->socket_fd >= 0)
        close(client->socket_fd);
    client->socket_fd = -1;
}

static void frame_tap_client_close(frame_tap_client_t *client)
{
    frame_tap_client_hang_up(client);
    free(client->picture);
    client->picture = NULL;
}

/* A read that never times out would hold the capture forever on a service that stopped answering,
 * and it would not even notice a stop request, because its signal handler restarts the read. */
static int frame_tap_connect(const char *socket_path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct timeval read_timeout = { .tv_sec = FRAME_TAP_READ_TIMEOUT_S };
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (socket_fd < 0)
        return -1;
    strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout)) < 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static bool frame_tap_read_exactly(int socket_fd, void *destination, size_t wanted_bytes)
{
    uint8_t *next_byte = destination;
    size_t bytes_left = wanted_bytes;

    while (bytes_left > 0) {
        ssize_t bytes_read = read(socket_fd, next_byte, bytes_left);
        if (bytes_read <= 0)
            return false;
        next_byte += bytes_read;
        bytes_left -= (size_t)bytes_read;
    }
    return true;
}

/* NULL when a whole picture of the expected size is in client->picture, otherwise what went wrong. */
static const char *frame_tap_fetch(frame_tap_client_t *client)
{
    const char frame_request = FRAME_TAP_FRAME_REQUEST;
    frame_tap_header_t header;

    if (client->socket_fd < 0)
        client->socket_fd = frame_tap_connect(client->socket_path);
    if (client->socket_fd < 0)
        return "the printer's video service is not serving frames yet";
    if (send(client->socket_fd, &frame_request, sizeof(frame_request), MSG_NOSIGNAL) != 1)
        return "the frame request could not be sent";
    if (!frame_tap_read_exactly(client->socket_fd, &header, sizeof(header)))
        return "no picture came back";
    if (!frame_tap_header_is_valid(&header, client->width, client->height))
        return "a picture of another size or shape came back";
    if (!frame_tap_read_exactly(client->socket_fd, client->picture, client->picture_bytes))
        return "the picture was cut off";
    return NULL;
}

/* 1 with a whole picture in frame_data, 0 when there is none this time. Trouble is reported once
 * when it starts, and once more when frames flow again, not once a second while it lasts. */
static int frame_tap_client_receive(frame_tap_client_t *client, void **frame_data, size_t *bytesused)
{
    const char *failure = frame_tap_fetch(client);

    if (failure == NULL) {
        if (client->in_trouble)
            log_printf("Frame tap: receiving frames\n");
        client->in_trouble = false;
        *frame_data = client->picture;
        *bytesused = client->picture_bytes;
        return 1;
    }
    if (!client->in_trouble)
        log_errorf("Frame tap: %s, retrying\n", failure);
    client->in_trouble = true;
    frame_tap_client_hang_up(client);
    usleep(FRAME_TAP_RETRY_PAUSE_US);
    return 0;
}

#endif /* FRAME_TAP_CLIENT_H */
