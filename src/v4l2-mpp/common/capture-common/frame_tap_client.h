#ifndef FRAME_TAP_CLIENT_H
#define FRAME_TAP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "camera_photo_request.h"
#include "frame_tap_wire.h"
#include "freeze_recovery.h"
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

typedef enum {
    FRAME_TAP_PICTURE,
    FRAME_TAP_NOT_SERVING,
    FRAME_TAP_REQUEST_UNSENT,
    FRAME_TAP_CAMERA_SILENT,
    FRAME_TAP_WRONG_PICTURE,
    FRAME_TAP_PICTURE_CUT_OFF,
} frame_tap_outcome_t;

static const char *const frame_tap_outcome_messages[] = {
    [FRAME_TAP_NOT_SERVING] = "the printer's video service is not serving frames yet",
    [FRAME_TAP_REQUEST_UNSENT] = "the frame request could not be sent",
    [FRAME_TAP_CAMERA_SILENT] = "no picture came back",
    [FRAME_TAP_WRONG_PICTURE] = "a picture of another size or shape came back",
    [FRAME_TAP_PICTURE_CUT_OFF] = "the picture was cut off",
};

typedef struct {
    const char *socket_path;
    int socket_fd;
    uint32_t width;
    uint32_t height;
    uint8_t *picture;
    size_t picture_bytes;
    bool in_trouble;
    bool wakes_frozen_camera;
    freeze_recovery_t freeze_recovery;
    pid_t photo_publisher;
} frame_tap_client_t;

/* A client that was never opened, so closing it touches no socket and ends no process. */
#define FRAME_TAP_CLIENT_CLOSED { .socket_fd = -1, .photo_publisher = -1 }

static int frame_tap_client_open(frame_tap_client_t *client, const char *socket_path,
                                 uint32_t width, uint32_t height, bool wakes_frozen_camera)
{
    *client = (frame_tap_client_t)FRAME_TAP_CLIENT_CLOSED;
    client->socket_path = socket_path;
    client->width = width;
    client->height = height;
    client->picture_bytes = frame_tap_nv12_bytes(width, height);
    client->picture = malloc(client->picture_bytes);
    client->wakes_frozen_camera = wakes_frozen_camera;
    return client->picture ? 0 : -1;
}

static void frame_tap_client_hang_up(frame_tap_client_t *client)
{
    if (client->socket_fd >= 0)
        close(client->socket_fd);
    client->socket_fd = -1;
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

/* FRAME_TAP_PICTURE when a whole picture of the expected size is in client->picture. The tap hangs
 * up without a picture when its camera channel had none for it, so that is the camera falling
 * silent, told apart from the service not being there at all. */
static frame_tap_outcome_t frame_tap_fetch(frame_tap_client_t *client)
{
    const char frame_request = FRAME_TAP_FRAME_REQUEST;
    frame_tap_header_t header;

    if (client->socket_fd < 0)
        client->socket_fd = frame_tap_connect(client->socket_path);
    if (client->socket_fd < 0)
        return FRAME_TAP_NOT_SERVING;
    if (send(client->socket_fd, &frame_request, sizeof(frame_request), MSG_NOSIGNAL) != 1)
        return FRAME_TAP_REQUEST_UNSENT;
    if (!frame_tap_read_exactly(client->socket_fd, &header, sizeof(header)))
        return FRAME_TAP_CAMERA_SILENT;
    if (!frame_tap_header_is_valid(&header, client->width, client->height))
        return FRAME_TAP_WRONG_PICTURE;
    if (!frame_tap_read_exactly(client->socket_fd, client->picture, client->picture_bytes))
        return FRAME_TAP_PICTURE_CUT_OFF;
    return FRAME_TAP_PICTURE;
}

static time_t frame_tap_seconds_now(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

static freeze_recovery_t frame_tap_freeze_recovery_noted(freeze_recovery_t recovery,
                                                         frame_tap_outcome_t outcome, time_t now)
{
    if (outcome == FRAME_TAP_PICTURE)
        return freeze_recovery_picture_arrived(recovery);
    if (outcome == FRAME_TAP_CAMERA_SILENT)
        return freeze_recovery_camera_silent(recovery, now);
    return freeze_recovery_answer_without_picture(recovery);
}

static void frame_tap_log_photo_request(camera_photo_request_result_t request_result)
{
    if (request_result == CAMERA_PHOTO_REQUEST_SENT)
        log_printf("Frame tap: %s\n", camera_photo_request_messages[request_result]);
    else
        log_errorf("Frame tap: %s\n", camera_photo_request_messages[request_result]);
}

static void frame_tap_client_collect_photo_request(frame_tap_client_t *client)
{
    camera_photo_request_result_t request_result;

    if (client->photo_publisher <= 0)
        return;
    request_result = camera_photo_request_collect(client->photo_publisher);
    if (request_result == CAMERA_PHOTO_REQUEST_PENDING)
        return;
    client->photo_publisher = -1;
    frame_tap_log_photo_request(request_result);
}

/* A request that already finished is reported as it ended; only one still running is cut short. */
static void frame_tap_client_end_photo_request(frame_tap_client_t *client)
{
    frame_tap_client_collect_photo_request(client);
    if (client->photo_publisher <= 0)
        return;
    camera_photo_request_abandon(client->photo_publisher);
    client->photo_publisher = -1;
    frame_tap_log_photo_request(CAMERA_PHOTO_REQUEST_ENDED);
}

static void frame_tap_client_close(frame_tap_client_t *client)
{
    frame_tap_client_end_photo_request(client);
    frame_tap_client_hang_up(client);
    free(client->picture);
    client->picture = NULL;
}

static void frame_tap_client_ask_for_photo(frame_tap_client_t *client)
{
    frame_tap_client_end_photo_request(client);
    client->photo_publisher = camera_photo_request_send();
    if (client->photo_publisher < 0)
        frame_tap_log_photo_request(CAMERA_PHOTO_REQUEST_FAILED);
}

static void frame_tap_client_wake_frozen_camera(frame_tap_client_t *client,
                                                frame_tap_outcome_t outcome)
{
    time_t now = frame_tap_seconds_now();

    frame_tap_client_collect_photo_request(client);
    client->freeze_recovery = frame_tap_freeze_recovery_noted(client->freeze_recovery, outcome, now);
    if (!freeze_recovery_photo_is_due(client->freeze_recovery, now))
        return;
    client->freeze_recovery = freeze_recovery_photo_asked(client->freeze_recovery, now);
    frame_tap_client_ask_for_photo(client);
}

/* 1 with a whole picture in frame_data, 0 when there is none this time. Trouble is reported once
 * when it starts, and once more when frames flow again, not once a second while it lasts. */
static int frame_tap_client_receive(frame_tap_client_t *client, void **frame_data, size_t *bytesused)
{
    frame_tap_outcome_t outcome = frame_tap_fetch(client);

    if (client->wakes_frozen_camera)
        frame_tap_client_wake_frozen_camera(client, outcome);
    if (outcome == FRAME_TAP_PICTURE) {
        if (client->in_trouble)
            log_printf("Frame tap: receiving frames\n");
        client->in_trouble = false;
        *frame_data = client->picture;
        *bytesused = client->picture_bytes;
        return 1;
    }
    if (!client->in_trouble)
        log_errorf("Frame tap: %s, retrying\n", frame_tap_outcome_messages[outcome]);
    client->in_trouble = true;
    frame_tap_client_hang_up(client);
    usleep(FRAME_TAP_RETRY_PAUSE_US);
    return 0;
}

#endif /* FRAME_TAP_CLIENT_H */
