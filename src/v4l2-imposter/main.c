#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/videodev2.h>

#define MAX_BUFFERS 4
#define MAX_BUFFER_SIZE (4 * 1024 * 1024)

#define FOURCC_ARGS(f) (f) & 0xff, ((f) >> 8) & 0xff, ((f) >> 16) & 0xff, ((f) >> 24) & 0xff
#define FOURCC_FMT "%c%c%c%c"

#define LOG_PREFIX "[v4l2-mpp-injector] "
#define LOG_DEBUG(fmt, ...) do { if (config_debug) fprintf(stderr, LOG_PREFIX "DEBUG[%d]: " fmt "\n", getpid(), ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...) fprintf(stderr, LOG_PREFIX "INFO[%d]: " fmt "\n", getpid(), ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, LOG_PREFIX "ERROR[%d]: " fmt "\n", getpid(), ##__VA_ARGS__)

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;

static char config_socket_path[256] = "";
static char config_device[256] = "/dev/video0";
static int config_width = 1920;
static int config_height = 1080;
static uint32_t config_format = V4L2_PIX_FMT_MJPEG;
static char config_format_name[32] = "MJPEG";
static int config_debug = 0;
static int config_socket_timeout = 1000;

static int frame_pipe_read_fd = -1;
static int frame_pipe_write_fd = -1;
static int streaming = 0;
static int buffer_count = 0;

typedef struct {
    void *mapped;
    size_t size;
    size_t bytes_used;
    size_t offset;
    bool queued;
} buffer_t;

static buffer_t buffers[MAX_BUFFERS];

static void free_buffers(void)
{
    int slot;

    for (slot = 0; slot < MAX_BUFFERS; slot++) {
        if (buffers[slot].mapped != MAP_FAILED) {
            munmap(buffers[slot].mapped, buffers[slot].size);
            buffers[slot].mapped = MAP_FAILED;
        }
        buffers[slot].size = 0;
        buffers[slot].bytes_used = 0;
    }
    buffer_count = 0;
}

static bool load_config(void)
{
    const char *value;

    if (initialized)
        return true;

    // only intercept V4L2 calls inside the lmd/unisrv process
    char exe_path[256];
    ssize_t exe_path_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_path_len >= 0) {
        exe_path[exe_path_len] = '\0';
        const char *exe_name = strrchr(exe_path, '/');
        exe_name = exe_name ? exe_name + 1 : exe_path;
        if (strcmp(exe_name, "unisrv") != 0) {
            LOG_INFO("Current process is not unisrv (%s)", exe_name);
            return false;
        }
    }

    initialized = 1;

    value = getenv("V4L2_IMPOSTER_DEBUG");
    if (value && atoi(value))
        config_debug = 1;

    value = getenv("V4L2_IMPOSTER_SOCKET_PATH");
    if (value && strlen(value) > 0) {
        strncpy(config_socket_path, value, sizeof(config_socket_path) - 1);
        config_socket_path[sizeof(config_socket_path) - 1] = '\0';
    }

    value = getenv("V4L2_IMPOSTER_DEVICE");
    if (value && strlen(value) > 0) {
        strncpy(config_device, value, sizeof(config_device) - 1);
        config_device[sizeof(config_device) - 1] = '\0';
    }

    value = getenv("V4L2_IMPOSTER_WIDTH");
    if (value)
        config_width = atoi(value);

    value = getenv("V4L2_IMPOSTER_HEIGHT");
    if (value)
        config_height = atoi(value);

    value = getenv("V4L2_IMPOSTER_FORMAT");
    if (value) {
        strncpy(config_format_name, value, sizeof(config_format_name) - 1);
        config_format_name[sizeof(config_format_name) - 1] = '\0';
        if (strcasecmp(value, "MJPEG") == 0)
            config_format = V4L2_PIX_FMT_MJPEG;
        else if (strcasecmp(value, "JPEG") == 0)
            config_format = V4L2_PIX_FMT_JPEG;
        else if (strcasecmp(value, "YUYV") == 0)
            config_format = V4L2_PIX_FMT_YUYV;
        else if (strcasecmp(value, "NV12") == 0)
            config_format = V4L2_PIX_FMT_NV12;
        else {
            LOG_ERROR("Unsupported format: %s", value);
            return false;
        }
    }

    value = getenv("V4L2_IMPOSTER_SOCKET_TIMEOUT");
    if (value)
        config_socket_timeout = atoi(value);

    LOG_INFO("Config: socket=%s device=%s width=%d height=%d format=" FOURCC_FMT " timeout=%d",
              config_socket_path, config_device, config_width, config_height, FOURCC_ARGS(config_format), config_socket_timeout);
    return true;
}


static int handle_open(const char *path, int open_flags)
{
    int pipe_fds[2];

    (void)open_flags;

    if (!load_config()) {
        errno = EINVAL;
        return -1;
    }

    if (config_device[0] != '\0' && strcmp(path, config_device) != 0) {
        LOG_DEBUG("v4l2_open(%s): not target device %s", path, config_device);
        errno = ENOENT;
        return -1;
    }

    if (frame_pipe_read_fd >= 0) {
        LOG_ERROR("v4l2_open(%s): already open (fd %d)", path, frame_pipe_read_fd);
        errno = EBUSY;
        return -1;
    }

    if (pipe(pipe_fds) < 0) {
        LOG_ERROR("v4l2_open(%s): pipe failed: %s", path, strerror(errno));
        return -1;
    }

    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

    frame_pipe_read_fd = pipe_fds[0];
    frame_pipe_write_fd = pipe_fds[1];
    LOG_DEBUG("v4l2_open(%s) -> fd %d", path, frame_pipe_read_fd);

    return frame_pipe_read_fd;
}

int v4l2_open(const char *path, int open_flags, ...)
{
    pthread_mutex_lock(&state_lock);
    int result_fd = handle_open(path, open_flags);
    pthread_mutex_unlock(&state_lock);
    return result_fd;
}

int v4l2_close(int fd)
{
    if (!initialized) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&state_lock);

    if (fd == frame_pipe_read_fd) {
        LOG_DEBUG("v4l2_close(%d)", fd);
        free_buffers();
        close(frame_pipe_read_fd);
        close(frame_pipe_write_fd);
        frame_pipe_read_fd = -1;
        frame_pipe_write_fd = -1;
        streaming = 0;
    }

    pthread_mutex_unlock(&state_lock);

    return 0;
}

static int connect_to_socket(void)
{
    struct sockaddr_un address;
    int sock_fd;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, config_socket_path, sizeof(address.sun_path) - 1);
    address.sun_path[sizeof(address.sun_path) - 1] = '\0';

    if (connect(sock_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        LOG_ERROR("Failed to connect to %s: %s", config_socket_path, strerror(errno));
        close(sock_fd);
        return -1;
    }

    LOG_DEBUG("Connected to socket %s (fd=%d)", config_socket_path, sock_fd);
    return sock_fd;
}

static ssize_t read_once(int fd, void *dest, size_t count, int timeout_ms)
{
    struct pollfd poll_fd;
    ssize_t bytes_read;
    int poll_result;

    for (;;) {
        poll_fd.fd = fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;

        poll_result = poll(&poll_fd, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (poll_result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (poll_fd.revents & POLLERR) {
            errno = ECONNRESET;
            return -1;
        }
        if (poll_fd.revents & POLLHUP)
            return 0;

        bytes_read = read(fd, dest, count);
        if (bytes_read < 0 && errno == EINTR)
            continue;
        return bytes_read;
    }
}

static ssize_t read_fully(int fd, void *dest, size_t count, int timeout_ms)
{
    size_t total = 0;
    ssize_t chunk;

    while (total < count) {
        chunk = read_once(fd, (char *)dest + total, count - total, timeout_ms);
        if (chunk < 0)
            return -1;
        if (chunk == 0)
            break;
        total += chunk;
    }
    return total;
}

static int fetch_frame(buffer_t *buffer)
{
    ssize_t frame_bytes;
    int sock_fd;
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    sock_fd = connect_to_socket();
    if (sock_fd < 0)
        return -1;

    frame_bytes = read_fully(sock_fd, buffer->mapped, buffer->size, config_socket_timeout);
    close(sock_fd);

    if (frame_bytes < 0) {
        LOG_ERROR("Failed to read frame data: %s", strerror(errno));
        return -1;
    }
    if (frame_bytes == 0) {
        LOG_ERROR("No data received");
        return -1;
    }
    if ((size_t)frame_bytes > buffer->size) {
        LOG_ERROR("Frame size %zd exceeds buffer size %zu", frame_bytes, buffer->size);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);

    long elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000;

    buffer->bytes_used = frame_bytes;
    LOG_DEBUG("Fetched frame: %zd bytes in %ld ms", frame_bytes, elapsed_ms);
    return 0;
}

static int handle_querycap(struct v4l2_capability *capability)
{
    memset(capability, 0, sizeof(*capability));
    strncpy((char *)capability->driver, "v4l2-imposter", sizeof(capability->driver) - 1);
    snprintf((char *)capability->card, sizeof(capability->card), "Imposter %s", config_device);
    strncpy((char *)capability->bus_info, config_socket_path, sizeof(capability->bus_info) - 1);
    capability->version = 0x00050400;
    capability->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_DEVICE_CAPS;
    capability->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    LOG_DEBUG("QUERYCAP: driver=%s card=%s bus_info=%s",
              capability->driver, capability->card, capability->bus_info);
    return 0;
}

static int handle_enum_fmt(struct v4l2_fmtdesc *format_desc)
{
    if (format_desc->index > 0) {
        LOG_DEBUG("ENUM_FMT: invalid index %u", format_desc->index);
        return -EINVAL;
    }
    if (format_desc->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("ENUM_FMT: invalid type %u", format_desc->type);
        return -EINVAL;
    }

    memset(format_desc, 0, sizeof(*format_desc));
    format_desc->index = 0;
    format_desc->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format_desc->pixelformat = config_format;
    if (config_format == V4L2_PIX_FMT_MJPEG || config_format == V4L2_PIX_FMT_JPEG)
        format_desc->flags = V4L2_FMT_FLAG_COMPRESSED;
    strncpy((char *)format_desc->description, config_format_name, sizeof(format_desc->description) - 1);
    LOG_DEBUG("ENUM_FMT index=%u format=%s", format_desc->index, config_format_name);
    return 0;
}

static int handle_g_fmt(struct v4l2_format *format)
{
    if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("G_FMT: invalid type %u", format->type);
        return -EINVAL;
    }

    format->fmt.pix.width = config_width;
    format->fmt.pix.height = config_height;
    format->fmt.pix.pixelformat = config_format;
    format->fmt.pix.field = V4L2_FIELD_NONE;
    format->fmt.pix.bytesperline = 0;
    format->fmt.pix.sizeimage = config_width * config_height * 2;
    if (format->fmt.pix.sizeimage > MAX_BUFFER_SIZE) {
        LOG_ERROR("G_FMT: sizeimage overflow");
        return -EINVAL;
    }
    format->fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
    LOG_DEBUG("G_FMT: %dx%d fmt=" FOURCC_FMT, config_width, config_height, FOURCC_ARGS(config_format));
    return 0;
}

static int handle_s_fmt(struct v4l2_format *format)
{
    if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("S_FMT: invalid type %u", format->type);
        return -EINVAL;
    }

    if (format->fmt.pix.width != (unsigned)config_width ||
        format->fmt.pix.height != (unsigned)config_height) {
        LOG_ERROR("S_FMT: requested %ux%u but configured %dx%d",
                  format->fmt.pix.width, format->fmt.pix.height, config_width, config_height);
        return -EINVAL;
    }

    if (format->fmt.pix.pixelformat != config_format) {
        LOG_ERROR("S_FMT: requested format " FOURCC_FMT " but configured " FOURCC_FMT,
                  FOURCC_ARGS(format->fmt.pix.pixelformat), FOURCC_ARGS(config_format));
        return -EINVAL;
    }

    format->fmt.pix.field = V4L2_FIELD_NONE;
    format->fmt.pix.bytesperline = 0;
    format->fmt.pix.sizeimage = config_width * config_height * 2;
    if (format->fmt.pix.sizeimage > MAX_BUFFER_SIZE) {
        LOG_ERROR("S_FMT: sizeimage overflow");
        return -EINVAL;
    }
    format->fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
    LOG_DEBUG("S_FMT: %dx%d fmt=" FOURCC_FMT, config_width, config_height, FOURCC_ARGS(config_format));
    return 0;
}

static int handle_reqbufs(struct v4l2_requestbuffers *request)
{
    int slot;
    size_t buffer_size;

    if (request->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("REQBUFS: invalid type %u", request->type);
        return -EINVAL;
    }
    if (request->memory != V4L2_MEMORY_MMAP) {
        LOG_DEBUG("REQBUFS: invalid memory %u", request->memory);
        return -EINVAL;
    }

    free_buffers();

    if (request->count == 0)
        return 0;

    if (request->count > MAX_BUFFERS)
        request->count = MAX_BUFFERS;

    buffer_size = config_width * config_height * 2;
    if (buffer_size > MAX_BUFFER_SIZE) {
        LOG_ERROR("REQBUFS: buffer size overflow");
        return -EINVAL;
    }

    size_t offset = 0;

    for (slot = 0; slot < (int)request->count; slot++) {
        buffers[slot].mapped = MAP_FAILED;
        buffers[slot].size = buffer_size;
        buffers[slot].bytes_used = 0;
        buffers[slot].offset = offset;
        buffers[slot].queued = false;
        LOG_DEBUG("The buffer %d: size=%zu offset=0x%zx", slot, buffer_size, offset);
        offset += buffer_size;
    }

    buffer_count = slot;
    request->count = buffer_count;
    LOG_DEBUG("REQBUFS: allocated %d buffers (%zu bytes each)", buffer_count, buffer_size);
    return 0;
}

static int handle_querybuf(struct v4l2_buffer *buffer)
{
    if (buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("QUERYBUF: invalid type %u", buffer->type);
        return -EINVAL;
    }
    if (buffer->index >= (unsigned)buffer_count) {
        LOG_DEBUG("QUERYBUF: invalid index %u (count=%d)", buffer->index, buffer_count);
        return -EINVAL;
    }

    buffer->memory = V4L2_MEMORY_MMAP;
    buffer->length = buffers[buffer->index].size;
    buffer->m.offset = buffers[buffer->index].offset;
    buffer->flags = 0;
    LOG_DEBUG("QUERYBUF: index=%d offset=0x%x length=%u",
              buffer->index, buffer->m.offset, buffer->length);
    return 0;
}

static int handle_qbuf(struct v4l2_buffer *buffer)
{
    if (buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("QBUF: invalid type %u", buffer->type);
        return -EINVAL;
    }
    if (buffer->index >= (unsigned)buffer_count) {
        LOG_DEBUG("QBUF: invalid index %u (count=%d)", buffer->index, buffer_count);
        return -EINVAL;
    }
    if (buffers[buffer->index].mapped == MAP_FAILED) {
        LOG_DEBUG("QBUF: buffer %u not allocated", buffer->index);
        return -EINVAL;
    }
    if (buffers[buffer->index].queued) {
        LOG_DEBUG("QBUF: buffer %u already queued", buffer->index);
        return -EINVAL;
    }

    buffers[buffer->index].queued = true;

    if (streaming) {
        uint8_t slot = buffer->index;
        if (write(frame_pipe_write_fd, &slot, 1) != 1) {
            LOG_DEBUG("QBUF: write to pipe failed");
            buffers[buffer->index].queued = false;
            return -EIO;
        }
    }
    LOG_DEBUG("QBUF: index=%d", buffer->index);
    return 0;
}

static int handle_dqbuf(struct v4l2_buffer *buffer)
{
    uint8_t slot;
    ssize_t bytes_read;

    if (buffer->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("DQBUF: invalid type %u", buffer->type);
        return -EINVAL;
    }
    if (!streaming) {
        LOG_DEBUG("QBUF: not streaming");
        return -EINVAL;
    }

    bytes_read = read(frame_pipe_read_fd, &slot, 1);
    if (bytes_read != 1) {
        if (bytes_read < 0 && errno == EAGAIN) {
            LOG_DEBUG("DQBUF: no queued buffer");
            return -EAGAIN;
        }
        LOG_DEBUG("DQBUF: read from pipe failed");
        return -EIO;
    }

    if (slot >= buffer_count) {
        LOG_DEBUG("DQBUF: invalid index %u from pipe", slot);
        return -EIO;
    }

    if (!buffers[slot].queued) {
        LOG_DEBUG("DQBUF: buffer %u not queued", slot);
        return -EINVAL;
    }

    if (fetch_frame(&buffers[slot]) < 0) {
        if (write(frame_pipe_write_fd, &slot, 1) != 1) {
            LOG_ERROR("DQBUF: failed to re-queue buffer %u", slot);
            buffers[slot].queued = false;
        }
        return -EIO;
    }

    buffers[slot].queued = false;

    buffer->index = slot;
    buffer->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer->memory = V4L2_MEMORY_MMAP;
    buffer->bytesused = buffers[slot].bytes_used;
    buffer->length = buffers[slot].size;
    buffer->m.offset = buffers[slot].offset;
    buffer->flags = V4L2_BUF_FLAG_DONE;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    buffer->timestamp.tv_sec = now.tv_sec;
    buffer->timestamp.tv_usec = now.tv_nsec / 1000;

    LOG_DEBUG("DQBUF: index=%u bytesused=%u", slot, buffer->bytesused);
    return 0;
}

static int handle_streamon(enum v4l2_buf_type *buf_type)
{
    if (*buf_type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("STREAMON: invalid type %u", *buf_type);
        return -EINVAL;
    }
    if (streaming) {
        LOG_DEBUG("STREAMON: already streaming");
        return -EINVAL;
    }

    for (int slot = 0; slot < buffer_count; slot++) {
        if (buffers[slot].mapped == MAP_FAILED) {
            LOG_DEBUG("STREAMON: buffer %d not allocated", slot);
            continue;
        }
        if (!buffers[slot].queued) {
            LOG_DEBUG("STREAMON: buffer %d not queued", slot);
            continue;
        }
        uint8_t slot_byte = slot;
        if (write(frame_pipe_write_fd, &slot_byte, 1) != 1) {
            LOG_DEBUG("STREAMON: write to pipe failed");
            return -EIO;
        }
    }

    streaming = 1;
    LOG_DEBUG("STREAMON");
    return 0;
}

static int handle_streamoff(enum v4l2_buf_type *buf_type)
{
    uint8_t slot;

    if (*buf_type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("STREAMOFF: invalid type %u", *buf_type);
        return -EINVAL;
    }
    if (!streaming) {
        LOG_DEBUG("STREAMOFF: not streaming");
        return -EINVAL;
    }

    streaming = 0;
    while (read(frame_pipe_read_fd, &slot, 1) == 1) {
        if (slot < buffer_count)
            buffers[slot].queued = false;
    }

    LOG_DEBUG("STREAMOFF");
    return 0;
}

static int handle_g_parm(struct v4l2_streamparm *stream_parm)
{
    if (stream_parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("G_PARM: invalid type %u", stream_parm->type);
        return -EINVAL;
    }

    memset(&stream_parm->parm, 0, sizeof(stream_parm->parm));
    stream_parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    stream_parm->parm.capture.timeperframe.numerator = 1;
    stream_parm->parm.capture.timeperframe.denominator = 30;
    LOG_DEBUG("G_PARM");
    return 0;
}

static int handle_s_parm(struct v4l2_streamparm *stream_parm)
{
    if (stream_parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_DEBUG("S_PARM: invalid type %u", stream_parm->type);
        return -EINVAL;
    }

    stream_parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    if (stream_parm->parm.capture.timeperframe.denominator == 0)
        stream_parm->parm.capture.timeperframe.denominator = 30;
    if (stream_parm->parm.capture.timeperframe.numerator == 0)
        stream_parm->parm.capture.timeperframe.numerator = 1;
    LOG_DEBUG("S_PARM");
    return 0;
}

static int handle_enum_framesizes(struct v4l2_frmsizeenum *frame_size)
{
    if (frame_size->index > 0) {
        LOG_DEBUG("ENUM_FRAMESIZES: invalid index %u", frame_size->index);
        return -EINVAL;
    }
    if (frame_size->pixel_format != config_format) {
        LOG_DEBUG("ENUM_FRAMESIZES: invalid format " FOURCC_FMT, FOURCC_ARGS(frame_size->pixel_format));
        return -EINVAL;
    }

    frame_size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
    frame_size->discrete.width = config_width;
    frame_size->discrete.height = config_height;
    LOG_DEBUG("ENUM_FRAMESIZES");
    return 0;
}

static int handle_enum_frameintervals(struct v4l2_frmivalenum *frame_interval)
{
    if (frame_interval->index > 0) {
        LOG_DEBUG("ENUM_FRAMEINTERVALS: invalid index %u", frame_interval->index);
        return -EINVAL;
    }

    frame_interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;
    frame_interval->discrete.numerator = 1;
    frame_interval->discrete.denominator = 30;
    LOG_DEBUG("ENUM_FRAMEINTERVALS");
    return 0;
}

int v4l2_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *ioctl_arg;
    int result = -ENOTTY;

    if (!initialized) {
        errno = EINVAL;
        return -1;
    }

    va_start(args, request);
    ioctl_arg = va_arg(args, void *);
    va_end(args);

    request &= 0xffffffff;

    pthread_mutex_lock(&state_lock);
    if (fd != frame_pipe_read_fd || frame_pipe_read_fd < 0) {
        pthread_mutex_unlock(&state_lock);
        errno = EBADF;
        return -1;
    }

    switch (request) {
    case VIDIOC_QUERYCAP:
        result = handle_querycap(ioctl_arg);
        break;
    case VIDIOC_ENUM_FMT:
        result = handle_enum_fmt(ioctl_arg);
        break;
    case VIDIOC_G_FMT:
        result = handle_g_fmt(ioctl_arg);
        break;
    case VIDIOC_S_FMT:
    case VIDIOC_TRY_FMT:
        result = handle_s_fmt(ioctl_arg);
        break;
    case VIDIOC_REQBUFS:
        result = handle_reqbufs(ioctl_arg);
        break;
    case VIDIOC_QUERYBUF:
        result = handle_querybuf(ioctl_arg);
        break;
    case VIDIOC_QBUF:
        result = handle_qbuf(ioctl_arg);
        break;
    case VIDIOC_DQBUF:
        result = handle_dqbuf(ioctl_arg);
        break;
    case VIDIOC_STREAMON:
        result = handle_streamon(ioctl_arg);
        break;
    case VIDIOC_STREAMOFF:
        result = handle_streamoff(ioctl_arg);
        break;
    case VIDIOC_G_PARM:
        result = handle_g_parm(ioctl_arg);
        break;
    case VIDIOC_S_PARM:
        result = handle_s_parm(ioctl_arg);
        break;
    case VIDIOC_ENUM_FRAMESIZES:
        result = handle_enum_framesizes(ioctl_arg);
        break;
    case VIDIOC_ENUM_FRAMEINTERVALS:
        result = handle_enum_frameintervals(ioctl_arg);
        break;
    case VIDIOC_G_INPUT:
        *(int *)ioctl_arg = 0;
        result = 0;
        break;
    case VIDIOC_S_INPUT:
        result = 0;
        break;
    case VIDIOC_ENUMINPUT:
        {
            struct v4l2_input *input = ioctl_arg;
            if (input->index > 0) {
                result = -EINVAL;
            } else {
                memset(input, 0, sizeof(*input));
                input->index = 0;
                strncpy((char *)input->name, "Camera", sizeof(input->name) - 1);
                input->type = V4L2_INPUT_TYPE_CAMERA;
                result = 0;
            }
        }
        break;
    default:
        LOG_DEBUG("Unhandled ioctl 0x%lx", request);
        result = -ENOTTY;
        break;
    }

    pthread_mutex_unlock(&state_lock);

    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

static void *handle_mmap(void *start, size_t length, int prot, int flags, int fd, int64_t offset)
{
    (void)start;
    (void)length;
    (void)prot;
    (void)flags;

    LOG_DEBUG("v4l2_mmap(fd=%d, start=%p, length=%zu, offset=0x%lx)", fd, start, length, (unsigned long)offset);

    if (!initialized) {
        errno = EINVAL;
        return NULL;
    }

    if (start != NULL) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    if (fd != frame_pipe_read_fd) {
        errno = EBADF;
        return MAP_FAILED;
    }

    for (int slot = 0; slot < buffer_count; slot++) {
        if (buffers[slot].offset != (size_t)offset)
            continue;
        if (buffers[slot].size != length) {
            LOG_ERROR("v4l2_mmap: buffer %d size mismatch (expected %zu, got %zu)",
                      slot, buffers[slot].size, length);
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (buffers[slot].mapped == MAP_FAILED) {
            LOG_DEBUG("v4l2_mmap: allocating buffer %d of size %zu", slot, buffers[slot].size);
            buffers[slot].mapped = mmap(NULL, buffers[slot].size, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        if (buffers[slot].mapped == MAP_FAILED) {
            LOG_ERROR("v4l2_mmap: failed to allocate buffer %d", slot);
            break;
        }
        LOG_DEBUG("v4l2_mmap: returning buffer %d at %p", slot, buffers[slot].mapped);
        return buffers[slot].mapped;
    }

    errno = EINVAL;
    return MAP_FAILED;
}

void *v4l2_mmap(void *start, size_t length, int prot, int flags, int fd, int64_t offset)
{
    (void)start;
    (void)length;
    (void)prot;
    (void)flags;

    pthread_mutex_lock(&state_lock);
    void *mapped = handle_mmap(start, length, prot, flags, fd, offset);
    pthread_mutex_unlock(&state_lock);
    return mapped;
}

static int handle_munmap(void *start, size_t length)
{
    int slot;

    (void)length;

    if (!initialized) {
        errno = EINVAL;
        return -1;
    }

    for (slot = 0; slot < buffer_count; slot++) {
        if (buffers[slot].mapped == start && buffers[slot].mapped != MAP_FAILED) {
            if (buffers[slot].queued) {
                LOG_DEBUG("v4l2_munmap: buffer %d is queued, cannot unmap", slot);
                errno = EINVAL;
                return -1;
            }

            munmap(buffers[slot].mapped, buffers[slot].size);
            LOG_DEBUG("v4l2_munmap: unmapped buffer %d at %p", slot, buffers[slot].mapped);
            buffers[slot].mapped = MAP_FAILED;
            return 0;
        }
    }

    errno = EINVAL;
    return -1;
}

int v4l2_munmap(void *start, size_t length)
{
    (void)length;

    pthread_mutex_lock(&state_lock);
    int result = handle_munmap(start, length);
    pthread_mutex_unlock(&state_lock);
    return result;
}
