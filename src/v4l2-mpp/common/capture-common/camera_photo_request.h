#ifndef CAMERA_PHOTO_REQUEST_H
#define CAMERA_PHOTO_REQUEST_H


#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Closing every file the publisher would inherit is a GNU extension of posix_spawn, declared only
 * when _GNU_SOURCE was defined before the first system header. */
#ifndef __USE_GNU
#error "define _GNU_SOURCE before the first include to use camera_photo_request.h"
#endif

/* The printer's video service takes photo requests from the printer's own message broker, the same
 * way the Snapmaker app asks for one. The photo goes to a file of ours that nothing reads.
 *
 * The publisher runs directly, never through a shell, and is never waited for while it runs: the
 * capture's only loop must not stop on a broker that does not answer. Its result is collected
 * later, and one still running when the next photo is due is ended, so none is left behind.
 *
 * It gets none of the capture's files: its log output, the viewers' connections and the sockets
 * the capture listens on would otherwise stay open for as long as it ran, so a viewer would not see
 * the capture hang up and a capture that exited could not be restarted.
 *
 * The request's id is 0 because Klipper reads the service's answers too, and matches them to its own
 * requests by id; it numbers those from 1, so it never takes this answer for one of its own. */
#define CAMERA_PHOTO_PUBLISHER "/usr/bin/mosquitto_pub"
#define CAMERA_PHOTO_REQUEST                                                                      \
    "{\"jsonrpc\":\"2.0\",\"method\":\"camera.take_a_photo\",\"params\":{\"reason\":\"debug\","    \
    "\"timestamp\":false,\"filepath\":\"/tmp/capture-mipi-wake-photo.jpg\"},\"id\":0}"

typedef enum {
    CAMERA_PHOTO_REQUEST_PENDING,
    CAMERA_PHOTO_REQUEST_SENT,
    CAMERA_PHOTO_REQUEST_FAILED,
    CAMERA_PHOTO_REQUEST_ENDED,
} camera_photo_request_result_t;

static const char *const camera_photo_request_messages[] = {
    [CAMERA_PHOTO_REQUEST_SENT] =
        "the camera sent nothing for a while, a photo was asked for to wake it",
    [CAMERA_PHOTO_REQUEST_FAILED] =
        "the camera sent nothing for a while, and the photo to wake it could not be asked for",
    [CAMERA_PHOTO_REQUEST_ENDED] =
        "the last request for a photo to wake the camera did not finish, so it was ended",
};

static bool camera_photo_request_files_shut_out(posix_spawn_file_actions_t *publisher_files)
{
    return posix_spawn_file_actions_addopen(publisher_files, STDIN_FILENO, "/dev/null",
                                            O_RDONLY, 0) == 0 &&
           posix_spawn_file_actions_addopen(publisher_files, STDOUT_FILENO, "/dev/null",
                                            O_WRONLY, 0) == 0 &&
           posix_spawn_file_actions_adddup2(publisher_files, STDOUT_FILENO, STDERR_FILENO) == 0 &&
           posix_spawn_file_actions_addclosefrom_np(publisher_files, STDERR_FILENO + 1) == 0;
}

/* The publisher's process, or -1 when it could not be started. */
static pid_t camera_photo_request_send(void)
{
    char *const publish_command[] = {
        "mosquitto_pub", "-h", "127.0.0.1", "-p", "1883", "-t", "camera/request",
        "-m", CAMERA_PHOTO_REQUEST, NULL,
    };
    posix_spawn_file_actions_t publisher_files;
    pid_t publisher;
    bool started;

    if (posix_spawn_file_actions_init(&publisher_files) != 0)
        return -1;
    started = camera_photo_request_files_shut_out(&publisher_files) &&
              posix_spawn(&publisher, CAMERA_PHOTO_PUBLISHER, &publisher_files, NULL,
                          publish_command, environ) == 0;
    posix_spawn_file_actions_destroy(&publisher_files);
    return started ? publisher : -1;
}

static camera_photo_request_result_t camera_photo_request_collect(pid_t publisher)
{
    int publisher_status;
    pid_t finished = waitpid(publisher, &publisher_status, WNOHANG);

    if (finished == 0)
        return CAMERA_PHOTO_REQUEST_PENDING;
    if (finished != publisher)
        return CAMERA_PHOTO_REQUEST_FAILED;
    if (WIFEXITED(publisher_status) && WEXITSTATUS(publisher_status) == 0)
        return CAMERA_PHOTO_REQUEST_SENT;
    return CAMERA_PHOTO_REQUEST_FAILED;
}

static void camera_photo_request_abandon(pid_t publisher)
{
    kill(publisher, SIGKILL);
    waitpid(publisher, NULL, 0);
}

#endif /* CAMERA_PHOTO_REQUEST_H */
