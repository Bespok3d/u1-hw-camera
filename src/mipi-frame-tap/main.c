/* The frame tap: preloaded into the printer's own video service on firmware where that service
 * keeps the MIPI camera to itself. The service still takes its photos and its timelapse; the tap
 * borrows a picture from the same camera channel whenever the capture asks for one, so our streams
 * keep working without opening the camera device the service needs.
 *
 * It does nothing at all unless MIPI_FRAME_TAP_SOCK names the socket to serve on.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "../log-file-cap/log_file_cap.h"
#include "frame_tap_wire.h"
#include "strided_nv12.h"

#define LOG_PREFIX "[mipi-frame-tap] "
#define LOG_LINE_MAX 512

/* Channel 1 is the camera's main path, the full size picture the service's photos come from. */
#define TAPPED_CHANNEL 1
#define FRAME_WAIT_MS 1000
#define STOP_CHECK_MS 500
#define CLIENT_SEND_TIMEOUT_S 2

/* Room for the pipeline's whole frame description, which is larger than the part read here. */
#define FRAME_INFO_BYTES 1024

/* The pipeline's own values for an uncompressed NV12 picture. */
#define ROCKIT_PIXEL_FORMAT_NV12 0
#define ROCKIT_COMPRESS_MODE_NONE 0

/* The leading fields of the pipeline's frame description, in its own order. The pipeline's
 * headers are not shipped with the printer, so the layout is written out here and pinned to the
 * offsets the shipped library uses. */
typedef struct {
    void *memory_block;
    uint32_t width;
    uint32_t height;
    uint32_t stride_width;
    uint32_t stride_height;
    int32_t field;
    int32_t pixel_format;
    int32_t video_format;
    int32_t compress_mode;
    int32_t dynamic_range;
    int32_t colour_gamut;
} rockit_frame_head_t;

_Static_assert(offsetof(rockit_frame_head_t, width) == 0x08, "the frame width moved");
_Static_assert(offsetof(rockit_frame_head_t, pixel_format) == 0x1C, "the pixel format moved");
_Static_assert(offsetof(rockit_frame_head_t, compress_mode) == 0x24, "the compress mode moved");
_Static_assert(sizeof(rockit_frame_head_t) == 0x30, "the frame head changed size");

typedef int (*channel_switch_fn)(int pipe, int channel);
typedef int (*get_channel_frame_fn)(int pipe, int channel, void *frame_info, int wait_ms);
typedef int (*release_channel_frame_fn)(int pipe, int channel, const void *frame_info);
typedef void *(*block_address_fn)(void *memory_block);
typedef uint64_t (*block_size_fn)(void *memory_block);
typedef int (*flush_cache_fn)(void *memory_block, int read_only);

typedef struct {
    get_channel_frame_fn get_frame;
    release_channel_frame_fn release_frame;
    block_address_fn block_address;
    block_size_fn block_size;
    flush_cache_fn flush_cache;
} rockit_calls_t;

typedef struct {
    uint8_t *bytes;
    size_t capacity;
    size_t length;
} outbox_t;

enum {
    PROBLEM_NO_FRAME = 1 << 0,
    PROBLEM_UNEXPECTED_FORMAT = 1 << 1,
    PROBLEM_UNPACKABLE = 1 << 2,
    PROBLEM_UNMAPPED = 1 << 3,
    PROBLEM_NO_MEMORY = 1 << 4,
};

/* The service starts and stops the camera channel from its own threads. state_lock keeps a start
 * and a stop from crossing; the rest is owned by the serving thread while it runs. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t serving_thread;
static bool serving = false;
static atomic_bool stop_requested;

static int tapped_pipe;
static int listener_fd = -1;
static const char *listener_path;
static rockit_calls_t rockit;
static unsigned int reported_problems;

/* The service runs in the background with no terminal, so stderr goes nowhere. MIPI_FRAME_TAP_LOG
 * names a file instead. */
static FILE *log_file = NULL;

static void log_open(void)
{
    const char *log_path = getenv("MIPI_FRAME_TAP_LOG");

    if (log_file || !log_path)
        return;
    log_file = fopen(log_path, "ae");
}

static FILE *log_destination(void)
{
    return log_file ? log_file : stderr;
}

/* The service has many threads, so the shared clock `localtime` hands back is not ours to hold. */
static void clock_stamp_now(char *clock_stamp, size_t room)
{
    time_t logged_at = time(NULL);
    struct tm logged_at_local;

    if (localtime_r(&logged_at, &logged_at_local) == NULL) {
        clock_stamp[0] = '\0';
        return;
    }
    strftime(clock_stamp, room, "[%H:%M:%S]", &logged_at_local);
}

static void log_line(const char *level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Built whole and written once, so another thread's line is never spliced into the middle. */
static void log_line(const char *level, const char *format, ...)
{
    FILE *stream = log_destination();
    char clock_stamp[16];
    char message[LOG_LINE_MAX];
    va_list args;

    clock_stamp_now(clock_stamp, sizeof(clock_stamp));
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    fprintf(stream, "%s " LOG_PREFIX "%s[%d]: %s\n", clock_stamp, level, getpid(), message);
    fflush(stream);
    log_empty_when_full(stream);
}

#define LOG_INFO(fmt, ...) log_line("INFO", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_line("ERROR", fmt, ##__VA_ARGS__)

/* A fault that happens on every frame is worth one line, not thirty a second. */
#define LOG_PROBLEM_ONCE(problem, fmt, ...)                                  \
    do {                                                                     \
        if (!(reported_problems & (problem)))                                \
            LOG_ERROR(fmt, ##__VA_ARGS__);                                   \
        reported_problems |= (problem);                                      \
    } while (0)

/* Looked up rather than linked, so the tap loads into any process and stays inert where the
 * pipeline library is absent. Flushing the cache is the only optional call. */
static bool rockit_calls_resolve(rockit_calls_t *calls)
{
    calls->get_frame = (get_channel_frame_fn)dlsym(RTLD_DEFAULT, "RK_MPI_VI_GetChnFrame");
    calls->release_frame = (release_channel_frame_fn)dlsym(RTLD_DEFAULT, "RK_MPI_VI_ReleaseChnFrame");
    calls->block_address = (block_address_fn)dlsym(RTLD_DEFAULT, "RK_MPI_MB_Handle2VirAddr");
    calls->block_size = (block_size_fn)dlsym(RTLD_DEFAULT, "RK_MPI_MB_GetSize");
    calls->flush_cache = (flush_cache_fn)dlsym(RTLD_DEFAULT, "RK_MPI_SYS_MmzFlushCache");
    return calls->get_frame && calls->release_frame && calls->block_address && calls->block_size;
}

static int listener_open(const char *socket_path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (socket_fd < 0)
        return -1;
    strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
    unlink(socket_path);
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(socket_fd, 1) < 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

/* The send timeout keeps a capture that stopped reading from holding the camera channel. */
static int client_accept(void)
{
    struct pollfd listener_poll = { .fd = listener_fd, .events = POLLIN };
    struct timeval send_timeout = { .tv_sec = CLIENT_SEND_TIMEOUT_S };
    int client_fd;

    if (poll(&listener_poll, 1, STOP_CHECK_MS) <= 0)
        return -1;
    client_fd = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client_fd < 0)
        return -1;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    return client_fd;
}

typedef enum {
    REQUEST_NOT_YET,
    REQUEST_FRAME,
    REQUEST_CLIENT_GONE,
} client_request_t;

static client_request_t client_wait_for_request(int client_fd)
{
    struct pollfd client_poll = { .fd = client_fd, .events = POLLIN };
    int ready = poll(&client_poll, 1, STOP_CHECK_MS);
    char request;

    if (ready == 0 || (ready < 0 && errno == EINTR))
        return REQUEST_NOT_YET;
    if (read(client_fd, &request, sizeof(request)) != 1 || request != FRAME_TAP_FRAME_REQUEST)
        return REQUEST_CLIENT_GONE;
    return REQUEST_FRAME;
}

static bool outbox_reserve(outbox_t *outbox, size_t wanted_bytes)
{
    uint8_t *grown;

    if (wanted_bytes <= outbox->capacity)
        return true;
    grown = realloc(outbox->bytes, wanted_bytes);
    if (!grown)
        return false;
    outbox->bytes = grown;
    outbox->capacity = wanted_bytes;
    return true;
}

static bool frame_head_is_nv12(const rockit_frame_head_t *head)
{
    return head->pixel_format == ROCKIT_PIXEL_FORMAT_NV12 &&
           head->compress_mode == ROCKIT_COMPRESS_MODE_NONE;
}

/* The pipeline's write to the block is only certain to be visible to this CPU once the cache
 * over it is refreshed. */
static const uint8_t *frame_block_address(void *memory_block)
{
    if (rockit.flush_cache)
        rockit.flush_cache(memory_block, 1);
    return rockit.block_address(memory_block);
}

static bool outbox_fill(outbox_t *outbox, const rockit_frame_head_t *head)
{
    strided_nv12_t picture = {
        head->width, head->height, head->stride_width, head->stride_height,
        rockit.block_size(head->memory_block),
    };
    frame_tap_header_t header = {
        FRAME_TAP_MAGIC, head->width, head->height, frame_tap_nv12_bytes(head->width, head->height),
    };
    const uint8_t *block;

    if (!strided_nv12_is_packable(&picture)) {
        LOG_PROBLEM_ONCE(PROBLEM_UNPACKABLE, "picture %ux%u in stride %ux%u does not fit its block",
                         head->width, head->height, head->stride_width, head->stride_height);
        return false;
    }
    if (!outbox_reserve(outbox, sizeof(header) + header.frame_bytes)) {
        LOG_PROBLEM_ONCE(PROBLEM_NO_MEMORY, "no memory for a %u byte picture", header.frame_bytes);
        return false;
    }
    block = frame_block_address(head->memory_block);
    if (!block) {
        LOG_PROBLEM_ONCE(PROBLEM_UNMAPPED, "the picture's memory block could not be mapped");
        return false;
    }
    memcpy(outbox->bytes, &header, sizeof(header));
    strided_nv12_pack(&picture, block, outbox->bytes + sizeof(header));
    outbox->length = sizeof(header) + header.frame_bytes;
    return true;
}

static bool outbox_fill_from_frame_info(outbox_t *outbox, const void *frame_info)
{
    rockit_frame_head_t head;

    memcpy(&head, frame_info, sizeof(head));
    if (!frame_head_is_nv12(&head)) {
        LOG_PROBLEM_ONCE(PROBLEM_UNEXPECTED_FORMAT, "picture is format %d, compression %d, not NV12",
                         head.pixel_format, head.compress_mode);
        return false;
    }
    return outbox_fill(outbox, &head);
}

/* The frame goes back to the pipeline as soon as it is copied, so the service is never left
 * short of one while the copy travels to the capture. */
static bool outbox_take_frame(outbox_t *outbox)
{
    _Alignas(max_align_t) unsigned char frame_info[FRAME_INFO_BYTES] = { 0 };
    int frame_result = rockit.get_frame(tapped_pipe, TAPPED_CHANNEL, frame_info, FRAME_WAIT_MS);
    bool filled;

    if (frame_result != 0) {
        LOG_PROBLEM_ONCE(PROBLEM_NO_FRAME, "no frame from the camera channel (0x%x)",
                         (unsigned int)frame_result);
        return false;
    }
    filled = outbox_fill_from_frame_info(outbox, frame_info);
    rockit.release_frame(tapped_pipe, TAPPED_CHANNEL, frame_info);
    return filled;
}

/* An interrupted send has sent nothing and is simply sent again. */
static ssize_t send_some(int client_fd, const uint8_t *bytes, size_t length)
{
    ssize_t sent = send(client_fd, bytes, length, MSG_NOSIGNAL);

    if (sent < 0 && errno == EINTR)
        return 0;
    return sent;
}

static bool send_whole(int client_fd, const uint8_t *bytes, size_t length)
{
    while (length > 0) {
        ssize_t sent = send_some(client_fd, bytes, length);
        if (sent < 0 || atomic_load(&stop_requested))
            return false;
        bytes += sent;
        length -= (size_t)sent;
    }
    return true;
}

/* The client socket to keep serving, or -1 once it is closed. */
static int serve_client(int client_fd, outbox_t *outbox)
{
    client_request_t request = client_wait_for_request(client_fd);

    if (request == REQUEST_NOT_YET)
        return client_fd;
    if (request == REQUEST_FRAME && outbox_take_frame(outbox) &&
        send_whole(client_fd, outbox->bytes, outbox->length))
        return client_fd;
    close(client_fd);
    return -1;
}

static void *serve_frames(void *unused)
{
    outbox_t outbox = { 0 };
    int client_fd = -1;

    (void)unused;
    while (!atomic_load(&stop_requested))
        client_fd = client_fd < 0 ? client_accept() : serve_client(client_fd, &outbox);
    if (client_fd >= 0)
        close(client_fd);
    close(listener_fd);
    unlink(listener_path);
    listener_fd = -1;
    free(outbox.bytes);
    LOG_INFO("stopped serving frames");
    return NULL;
}

/* Called with state_lock held. Whether the serving thread is now running. */
static bool serving_launch(int pipe, const char *socket_path)
{
    log_open();
    if (!rockit_calls_resolve(&rockit)) {
        LOG_ERROR("the camera pipeline's frame calls are missing, not tapping");
        return false;
    }
    listener_fd = listener_open(socket_path);
    if (listener_fd < 0) {
        LOG_ERROR("cannot serve frames on %s: %s", socket_path, strerror(errno));
        return false;
    }
    tapped_pipe = pipe;
    listener_path = socket_path;
    reported_problems = 0;
    atomic_store(&stop_requested, false);
    if (pthread_create(&serving_thread, NULL, serve_frames, NULL) != 0) {
        LOG_ERROR("cannot start the serving thread");
        close(listener_fd);
        unlink(socket_path);
        listener_fd = -1;
        return false;
    }
    LOG_INFO("serving frames of pipe %d channel %d on %s", pipe, TAPPED_CHANNEL, socket_path);
    return true;
}

static void serving_start(int pipe)
{
    const char *socket_path = getenv("MIPI_FRAME_TAP_SOCK");

    if (!socket_path)
        return;
    pthread_mutex_lock(&state_lock);
    if (!serving)
        serving = serving_launch(pipe, socket_path);
    pthread_mutex_unlock(&state_lock);
}

/* The serving thread may be inside a frame call on this channel, so it is stopped and waited for
 * before the channel goes away under it. */
static void serving_stop(int pipe)
{
    pthread_mutex_lock(&state_lock);
    if (serving && tapped_pipe == pipe) {
        atomic_store(&stop_requested, true);
        pthread_join(serving_thread, NULL);
        serving = false;
    }
    pthread_mutex_unlock(&state_lock);
}

int RK_MPI_VI_EnableChn(int pipe, int channel)
{
    channel_switch_fn real_enable = (channel_switch_fn)dlsym(RTLD_NEXT, "RK_MPI_VI_EnableChn");
    int enable_result = real_enable(pipe, channel);

    if (enable_result == 0 && channel == TAPPED_CHANNEL)
        serving_start(pipe);
    return enable_result;
}

int RK_MPI_VI_DisableChn(int pipe, int channel)
{
    channel_switch_fn real_disable = (channel_switch_fn)dlsym(RTLD_NEXT, "RK_MPI_VI_DisableChn");

    if (channel == TAPPED_CHANNEL)
        serving_stop(pipe);
    return real_disable(pipe, channel);
}
