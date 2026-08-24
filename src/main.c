/*
 * wayland-zeroprint: Zero-Drop Hardware PrintScreen Daemon for Linux Wayland
 *
 * Direct KWin D-Bus Engine + Multi-Compositor Universal Engine
 *
 * Copyright (c) 2026 Nguyen Dong Quan <nguyendongquan247@gmail.com>
 * Licensed under the Apache License, Version 2.0
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <systemd/sd-bus.h>
#include <zlib.h>

#define VERSION "1.1.1"
#define MAX_EPOLL_EVENTS 64
#define DEBOUNCE_MS 350.0
#define SHM_PATH "/dev/shm/wayland_zeroprint.png"
#define SHM_TMP_PREFIX "/dev/shm/.wayland_zeroprint.tmp"
#define CAPTURE_TIMEOUT_MS 5000
#define PNG_OUTPUT_CHUNK_SIZE (128 * 1024)

#ifndef KEY_SYSRQ
#define KEY_SYSRQ 99
#endif
#ifndef KEY_PRINT
#define KEY_PRINT 210
#endif

typedef enum {
    BACKEND_AUTO = 0,
    BACKEND_KDE,
    BACKEND_GNOME,
    BACKEND_WLROOTS
} compositor_backend_t;

static volatile sig_atomic_t g_running = 1;
static int g_epoll_fd = -1;
static int g_inotify_fd = -1;
static compositor_backend_t g_backend = BACKEND_AUTO;
static sd_bus *g_user_bus = NULL;

/* Asynchronous Worker Queue */
static pthread_mutex_t g_work_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work_cond = PTHREAD_COND_INITIALIZER;
static bool g_work_pending = false;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    double scale;
} capture_metadata_t;

static capture_metadata_t g_last_capture = {0};

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

static void make_temp_path(char *buffer, size_t size) {
    snprintf(buffer, size, "%s.%ld", SHM_TMP_PREFIX, (long)getpid());
}

static bool write_all(FILE *f, const void *data, size_t size) {
    return fwrite(data, 1, size, f) == size;
}

static bool write_be32(FILE *f, uint32_t value) {
    const uint8_t bytes[4] = {
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value
    };
    return write_all(f, bytes, sizeof(bytes));
}

static bool write_png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t size) {
    if (!write_be32(f, size) || !write_all(f, type, 4)) {
        return false;
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (size > 0) {
        if (!write_all(f, data, size)) {
            return false;
        }
        crc = crc32(crc, data, size);
    }
    return write_be32(f, (uint32_t)crc);
}

static int qimage_bytes_per_pixel(uint32_t format) {
    switch (format) {
        case 4:  /* QImage::Format_RGB32 */
        case 5:  /* QImage::Format_ARGB32 */
        case 6:  /* QImage::Format_ARGB32_Premultiplied */
        case 16: /* QImage::Format_RGBX8888 */
        case 17: /* QImage::Format_RGBA8888 */
        case 18: /* QImage::Format_RGBA8888_Premultiplied */
            return 4;
        case 13: /* QImage::Format_RGB888 */
        case 29: /* QImage::Format_BGR888 */
            return 3;
        default:
            return 0;
    }
}

static uint8_t unpremultiply_channel(uint8_t color, uint8_t alpha) {
    if (alpha == 0 || alpha == 255) {
        return color;
    }
    unsigned value = ((unsigned)color * 255u + alpha / 2u) / alpha;
    return (uint8_t)(value > 255u ? 255u : value);
}

static void convert_qimage_row_to_rgba(uint8_t *dst, const uint8_t *src,
                                       uint32_t width, uint32_t format) {
    for (uint32_t x = 0; x < width; x++) {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a = 255;

        switch (format) {
            case 4: /* QImage::Format_RGB32: native-endian 0xffRRGGBB */
                b = src[0];
                g = src[1];
                r = src[2];
                src += 4;
                break;
            case 5: /* QImage::Format_ARGB32: native-endian 0xAARRGGBB */
            case 6: /* QImage::Format_ARGB32_Premultiplied */
                b = src[0];
                g = src[1];
                r = src[2];
                a = src[3];
                src += 4;
                if (format == 6) {
                    r = unpremultiply_channel(r, a);
                    g = unpremultiply_channel(g, a);
                    b = unpremultiply_channel(b, a);
                }
                break;
            case 16: /* QImage::Format_RGBX8888: byte-ordered RGBX */
                r = src[0];
                g = src[1];
                b = src[2];
                src += 4;
                break;
            case 17: /* QImage::Format_RGBA8888: byte-ordered RGBA */
            case 18: /* QImage::Format_RGBA8888_Premultiplied */
                r = src[0];
                g = src[1];
                b = src[2];
                a = src[3];
                src += 4;
                if (format == 18) {
                    r = unpremultiply_channel(r, a);
                    g = unpremultiply_channel(g, a);
                    b = unpremultiply_channel(b, a);
                }
                break;
            case 13: /* QImage::Format_RGB888 */
                r = src[0];
                g = src[1];
                b = src[2];
                src += 3;
                break;
            case 29: /* QImage::Format_BGR888 */
                b = src[0];
                g = src[1];
                r = src[2];
                src += 3;
                break;
            default:
                return;
        }

        *dst++ = r;
        *dst++ = g;
        *dst++ = b;
        *dst++ = a;
    }
}

static ssize_t read_exact_with_timeout(int fd, void *buffer, size_t size, int timeout_ms) {
    uint8_t *dst = buffer;
    size_t total = 0;
    double deadline = get_time_ms() + timeout_ms;

    while (total < size) {
        ssize_t n = read(fd, dst + total, size - total);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) {
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }

        int remaining_ms = (int)(deadline - get_time_ms());
        if (remaining_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN
        };
        int ready = poll(&pfd, 1, remaining_ms);
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            errno = EIO;
            return -1;
        }
    }

    return (ssize_t)total;
}

static bool write_png_from_qimage_pipe(const char *filename, int input_fd,
                                       const capture_metadata_t *meta) {
    const int bytes_per_pixel = qimage_bytes_per_pixel(meta->format);
    if (bytes_per_pixel == 0 ||
        meta->width == 0 || meta->height == 0 ||
        meta->width > 32768 || meta->height > 32768 ||
        meta->stride < meta->width * (uint32_t)bytes_per_pixel ||
        meta->stride > 512u * 1024u * 1024u) {
        errno = EINVAL;
        return false;
    }

    if (meta->height > SIZE_MAX / meta->stride) {
        errno = EOVERFLOW;
        return false;
    }
    const size_t raw_size = (size_t)meta->stride * meta->height;
    uint8_t *raw_pixels = mmap(NULL, raw_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw_pixels == MAP_FAILED) {
        return false;
    }

    /*
     * Drain KWin's non-blocking pipe before doing CPU-heavy PNG work. KWin's
     * writer can otherwise hit pipe backpressure and terminate with a short
     * frame while compression is in progress.
     */
    if (read_exact_with_timeout(input_fd, raw_pixels, raw_size,
                                CAPTURE_TIMEOUT_MS) < 0) {
        munmap(raw_pixels, raw_size);
        return false;
    }

    int output_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (output_fd < 0) {
        munmap(raw_pixels, raw_size);
        return false;
    }
    FILE *f = fdopen(output_fd, "wb");
    if (!f) {
        close(output_fd);
        munmap(raw_pixels, raw_size);
        return false;
    }

    const size_t rgba_row_size = (size_t)meta->width * 4;
    uint8_t *filtered_row = malloc(rgba_row_size + 1);
    uint8_t *output_chunk = malloc(PNG_OUTPUT_CHUNK_SIZE);
    bool ok = filtered_row && output_chunk;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    bool zlib_initialized = false;

    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13] = {
        (uint8_t)(meta->width >> 24),
        (uint8_t)(meta->width >> 16),
        (uint8_t)(meta->width >> 8),
        (uint8_t)meta->width,
        (uint8_t)(meta->height >> 24),
        (uint8_t)(meta->height >> 16),
        (uint8_t)(meta->height >> 8),
        (uint8_t)meta->height,
        8, 6, 0, 0, 0
    };

    if (ok) {
        ok = write_all(f, signature, sizeof(signature)) &&
             write_png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    }
    if (ok) {
        int zret = deflateInit2(&strm, Z_BEST_SPEED, Z_DEFLATED, 15, 8, Z_RLE);
        ok = (zret == Z_OK);
        zlib_initialized = ok;
    }

    for (uint32_t y = 0; ok && y < meta->height; y++) {
        filtered_row[0] = 1; /* PNG Sub filter. */
        convert_qimage_row_to_rgba(filtered_row + 1,
                                   raw_pixels + (size_t)y * meta->stride,
                                   meta->width, meta->format);
        for (size_t i = rgba_row_size; i-- > 4;) {
            filtered_row[i + 1] =
                (uint8_t)(filtered_row[i + 1] - filtered_row[i + 1 - 4]);
        }

        strm.next_in = filtered_row;
        strm.avail_in = (uInt)(rgba_row_size + 1);
        while (ok && strm.avail_in > 0) {
            strm.next_out = output_chunk;
            strm.avail_out = PNG_OUTPUT_CHUNK_SIZE;
            int zret = deflate(&strm, Z_NO_FLUSH);
            if (zret != Z_OK) {
                ok = false;
                break;
            }
            uint32_t produced = PNG_OUTPUT_CHUNK_SIZE - strm.avail_out;
            if (produced > 0) {
                ok = write_png_chunk(f, "IDAT", output_chunk, produced);
            }
        }
    }

    while (ok) {
        strm.next_out = output_chunk;
        strm.avail_out = PNG_OUTPUT_CHUNK_SIZE;
        int zret = deflate(&strm, Z_FINISH);
        uint32_t produced = PNG_OUTPUT_CHUNK_SIZE - strm.avail_out;
        if (produced > 0) {
            ok = write_png_chunk(f, "IDAT", output_chunk, produced);
        }
        if (!ok || zret == Z_STREAM_END) {
            break;
        }
        if (zret != Z_OK) {
            ok = false;
            break;
        }
    }

    if (ok) {
        ok = write_png_chunk(f, "IEND", NULL, 0);
    }

    if (zlib_initialized) {
        deflateEnd(&strm);
    }
    munmap(raw_pixels, raw_size);
    free(filtered_row);
    free(output_chunk);

    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(filename);
    }
    return ok;
}

static bool append_bool_option(sd_bus_message *message, const char *key, int value) {
    return sd_bus_message_open_container(message, 'e', "sv") >= 0 &&
           sd_bus_message_append(message, "s", key) >= 0 &&
           sd_bus_message_open_container(message, 'v', "b") >= 0 &&
           sd_bus_message_append(message, "b", value) >= 0 &&
           sd_bus_message_close_container(message) >= 0 &&
           sd_bus_message_close_container(message) >= 0;
}

static bool read_capture_metadata(sd_bus_message *reply, capture_metadata_t *meta) {
    bool raw_type = false;
    memset(meta, 0, sizeof(*meta));
    meta->scale = 1.0;

    if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0) {
        return false;
    }

    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key = NULL;
        char container_type = 0;
        const char *signature = NULL;

        if (sd_bus_message_read_basic(reply, 's', &key) < 0 ||
            sd_bus_message_peek_type(reply, &container_type, &signature) < 0 ||
            container_type != 'v' || !signature ||
            sd_bus_message_enter_container(reply, 'v', signature) < 0) {
            return false;
        }

        if (strcmp(key, "type") == 0 && signature[0] == 's') {
            const char *value = NULL;
            if (sd_bus_message_read_basic(reply, 's', &value) >= 0) {
                raw_type = value && strcmp(value, "raw") == 0;
            }
        } else if ((strcmp(key, "width") == 0 ||
                    strcmp(key, "height") == 0 ||
                    strcmp(key, "stride") == 0 ||
                    strcmp(key, "format") == 0) &&
                   (signature[0] == 'u' || signature[0] == 'i')) {
            uint32_t value = 0;
            if (signature[0] == 'u') {
                sd_bus_message_read_basic(reply, 'u', &value);
            } else {
                int32_t signed_value = 0;
                sd_bus_message_read_basic(reply, 'i', &signed_value);
                if (signed_value > 0) {
                    value = (uint32_t)signed_value;
                }
            }
            if (strcmp(key, "width") == 0) meta->width = value;
            else if (strcmp(key, "height") == 0) meta->height = value;
            else if (strcmp(key, "stride") == 0) meta->stride = value;
            else meta->format = value;
        } else if (strcmp(key, "scale") == 0 && signature[0] == 'd') {
            sd_bus_message_read_basic(reply, 'd', &meta->scale);
        } else {
            sd_bus_message_skip(reply, signature);
        }

        if (sd_bus_message_exit_container(reply) < 0 ||
            sd_bus_message_exit_container(reply) < 0) {
            return false;
        }
    }

    if (sd_bus_message_exit_container(reply) < 0) {
        return false;
    }

    const int bytes_per_pixel = qimage_bytes_per_pixel(meta->format);
    return raw_type &&
           bytes_per_pixel > 0 &&
           meta->width > 0 && meta->height > 0 &&
           meta->stride >= meta->width * (uint32_t)bytes_per_pixel &&
           meta->scale > 0.0;
}

/*
 * KWin ScreenShot2 uses logical compositor coordinates. Requesting the DRM
 * mode as a CaptureArea therefore over-captures at fractional scale. The
 * native-resolution option is the supported way to ask KWin to render the
 * complete workspace at compositor-native resolution.
 */
static bool capture_kwin_direct_dbus(void) {
    if (!g_user_bus && sd_bus_open_user(&g_user_bus) < 0) {
        return false;
    }

    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC) < 0) {
        return false;
    }
    int read_flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (read_flags < 0 ||
        fcntl(pipe_fds[0], F_SETFL, read_flags | O_NONBLOCK) < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *message = NULL;
    sd_bus_message *reply = NULL;
    capture_metadata_t meta;
    char temp_path[128];
    bool ok = false;
    make_temp_path(temp_path, sizeof(temp_path));

    int r = sd_bus_message_new_method_call(g_user_bus, &message,
                                           "org.kde.KWin",
                                           "/org/kde/KWin/ScreenShot2",
                                           "org.kde.KWin.ScreenShot2",
                                           "CaptureWorkspace");
    if (r < 0 ||
        sd_bus_message_open_container(message, 'a', "{sv}") < 0 ||
        !append_bool_option(message, "native-resolution", 1) ||
        sd_bus_message_close_container(message) < 0 ||
        sd_bus_message_append(message, "h", pipe_fds[1]) < 0) {
        goto cleanup;
    }

    r = sd_bus_call(g_user_bus, message, 2000000, &error, &reply);
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
    if (r < 0 || !read_capture_metadata(reply, &meta)) {
        goto cleanup;
    }
    /*
     * sd-bus keeps a duplicated copy of the passed descriptor in the
     * outgoing message. Release it before draining the pipe so EOF reflects
     * only KWin's writer.
     */
    sd_bus_message_unref(message);
    message = NULL;
    sd_bus_message_unref(reply);
    reply = NULL;

    unlink(temp_path);
    if (!write_png_from_qimage_pipe(temp_path, pipe_fds[0], &meta)) {
        goto cleanup;
    }
    if (rename(temp_path, SHM_PATH) < 0) {
        unlink(temp_path);
        goto cleanup;
    }

    g_last_capture = meta;
    ok = true;

cleanup:
    if (!ok) unlink(temp_path);
    if (pipe_fds[0] >= 0) close(pipe_fds[0]);
    if (pipe_fds[1] >= 0) close(pipe_fds[1]);
    sd_bus_error_free(&error);
    sd_bus_message_unref(message);
    sd_bus_message_unref(reply);
    return ok;
}

static compositor_backend_t detect_backend(void) {
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    const char *session = getenv("DESKTOP_SESSION");
    char combined[256] = {0};

    if (desktop) strncpy(combined, desktop, sizeof(combined) - 1);
    if (session) {
        strncat(combined, ":", sizeof(combined) - strlen(combined) - 1);
        strncat(combined, session, sizeof(combined) - strlen(combined) - 1);
    }

    for (char *p = combined; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    }

    if (strstr(combined, "kde") || strstr(combined, "plasma")) {
        return BACKEND_KDE;
    } else if (strstr(combined, "gnome")) {
        return BACKEND_GNOME;
    } else if (strstr(combined, "hyprland") || strstr(combined, "sway") || strstr(combined, "wayfire")) {
        return BACKEND_WLROOTS;
    }

    if (access("/usr/bin/spectacle", X_OK) == 0) return BACKEND_KDE;
    if (access("/usr/bin/grim", X_OK) == 0) return BACKEND_WLROOTS;
    if (access("/usr/bin/gdbus", X_OK) == 0) return BACKEND_GNOME;

    return BACKEND_KDE;
}

static bool is_valid_png_file(const char *path) {
    static const uint8_t expected[8] =
        {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t header[24];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    ssize_t count = read(fd, header, sizeof(header));
    close(fd);
    if (count != (ssize_t)sizeof(header) ||
        memcmp(header, expected, sizeof(expected)) != 0 ||
        memcmp(header + 12, "IHDR", 4) != 0) {
        return false;
    }

    uint32_t width = ((uint32_t)header[16] << 24) |
                     ((uint32_t)header[17] << 16) |
                     ((uint32_t)header[18] << 8) |
                     (uint32_t)header[19];
    uint32_t height = ((uint32_t)header[20] << 24) |
                      ((uint32_t)header[21] << 16) |
                      ((uint32_t)header[22] << 8) |
                      (uint32_t)header[23];
    return width > 0 && height > 0;
}

static void execute_capture_and_pipe(void) {
    bool captured = false;
    char temp_path[128];
    make_temp_path(temp_path, sizeof(temp_path));

    /* Prefer the direct KWin raw-frame path on KDE. */
    if (g_backend == BACKEND_KDE) {
        captured = capture_kwin_direct_dbus();
    }

    /* Fall back to the compositor's CLI and publish only a complete PNG. */
    if (!captured) {
        unlink(temp_path);
        int ret = -1;
        switch (g_backend) {
            case BACKEND_KDE: {
                pid_t pid = fork();
                if (pid == 0) {
                    int devnull = open("/dev/null", O_WRONLY);
                    if (devnull >= 0) {
                        dup2(devnull, STDERR_FILENO);
                        dup2(devnull, STDOUT_FILENO);
                        close(devnull);
                    }
                    char *args[] = {"spectacle", "-b", "-f", "-n", "-o", temp_path, NULL};
                    execvp("spectacle", args);
                    _exit(127);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                    ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                }
                break;
            }
            case BACKEND_GNOME: {
                pid_t pid = fork();
                if (pid == 0) {
                    int devnull = open("/dev/null", O_WRONLY);
                    if (devnull >= 0) {
                        dup2(devnull, STDERR_FILENO);
                        dup2(devnull, STDOUT_FILENO);
                        close(devnull);
                    }
                    char *args[] = {"gdbus", "call", "--session",
                                    "--dest", "org.gnome.Shell.Screenshot",
                                    "--object-path", "/org/gnome/Shell/Screenshot",
                                    "--method", "org.gnome.Shell.Screenshot.Screenshot",
                                    "true", "false", temp_path, NULL};
                    execvp("gdbus", args);
                    _exit(127);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                    ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                }
                break;
            }
            case BACKEND_WLROOTS:
            default: {
                pid_t pid = fork();
                if (pid == 0) {
                    int devnull = open("/dev/null", O_WRONLY);
                    if (devnull >= 0) {
                        dup2(devnull, STDERR_FILENO);
                        dup2(devnull, STDOUT_FILENO);
                        close(devnull);
                    }
                    char *args[] = {"grim", temp_path, NULL};
                    execvp("grim", args);
                    _exit(127);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                    ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                }
                break;
            }
        }
        if (ret != 0 || !is_valid_png_file(temp_path) ||
            rename(temp_path, SHM_PATH) < 0) {
            unlink(temp_path);
            return;
        }
    }

    /* Stream the atomically published PNG into the clipboard. */
    int fd = open(SHM_PATH, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        pid_t copy_pid = fork();
        if (copy_pid == 0) {
            dup2(fd, STDIN_FILENO);
            close(fd);

            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                dup2(devnull, STDOUT_FILENO);
                close(devnull);
            }

            char *args[] = {"wl-copy", "-t", "image/png", NULL};
            execvp("wl-copy", args);
            _exit(127);
        } else if (copy_pid > 0) {
            close(fd);
            int copy_status = 0;
            waitpid(copy_pid, &copy_status, 0);
        } else {
            close(fd);
        }
    }
}

static void *worker_thread_func(void *arg) {
    (void)arg;
    while (g_running) {
        pthread_mutex_lock(&g_work_mutex);
        while (!g_work_pending && g_running) {
            pthread_cond_wait(&g_work_cond, &g_work_mutex);
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_work_mutex);
            break;
        }
        g_work_pending = false;
        pthread_mutex_unlock(&g_work_mutex);

        execute_capture_and_pipe();
    }
    return NULL;
}

static void add_device(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;

    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
    }
}

static void scan_initial_devices(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            add_device(path);
        }
    }
    closedir(dir);
}

static void handle_inotify_events(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;

    while ((len = read(g_inotify_fd, buf, sizeof(buf))) > 0) {
        for (char *ptr = buf; ptr < buf + len;) {
            struct inotify_event *event = (struct inotify_event *)ptr;
            if (event->len > 0 && strncmp(event->name, "event", 5) == 0) {
                char path[512];
                snprintf(path, sizeof(path), "/dev/input/%s", event->name);
                if (event->mask & (IN_CREATE | IN_ATTRIB)) {
                    add_device(path);
                }
            }
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }
}

static int compare_doubles(const void *left, const void *right) {
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void run_benchmark(void) {
    enum { SAMPLE_COUNT = 5 };

    printf("===================================================================\n");
    printf("       WAYLAND-ZEROPRINT V%s CAPTURE BENCHMARK                    \n", VERSION);
    printf("===================================================================\n");

    double t0 = get_time_us();
    double t1 = get_time_us();
    printf("  Clock read overhead sample : %.2f µs\n", t1 - t0);

    if (g_backend == BACKEND_KDE) {
        double samples[SAMPLE_COUNT];
        size_t completed = 0;
        for (size_t i = 0; i < SAMPLE_COUNT; i++) {
            double start = get_time_us();
            bool ok = capture_kwin_direct_dbus();
            double end = get_time_us();
            if (!ok) {
                break;
            }
            samples[completed++] = (end - start) / 1000.0;
        }

        if (completed == SAMPLE_COUNT) {
            qsort(samples, SAMPLE_COUNT, sizeof(samples[0]), compare_doubles);
            printf("  KWin native capture + PNG : min %.2f / median %.2f / max %.2f ms (%d runs)\n",
                   samples[0], samples[SAMPLE_COUNT / 2],
                   samples[SAMPLE_COUNT - 1], SAMPLE_COUNT);
            printf("  Captured frame            : %ux%u, scale %.2f, stride %u, QImage format %u\n",
                   g_last_capture.width, g_last_capture.height,
                   g_last_capture.scale, g_last_capture.stride,
                   g_last_capture.format);
        } else {
            printf("  KWin native capture       : failed after %zu/%d runs\n",
                   completed, SAMPLE_COUNT);
        }
    }

    printf("  Detected compositor       : %s\n",
           (g_backend == BACKEND_KDE) ? "KDE Plasma 6 (Direct D-Bus + Spectacle Fallback)" :
           (g_backend == BACKEND_GNOME) ? "GNOME Shell (gdbus)" : "wlroots (grim)");
    printf("===================================================================\n");
}

int main(int argc, char **argv) {
    g_backend = detect_backend();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--benchmark") == 0 || strcmp(argv[i], "-b") == 0) {
            run_benchmark();
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("wayland-zeroprint version %s (Native C + Direct KWin Engine)\n", VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: wayland-zeroprint [options]\n");
            printf("Options:\n");
            printf("  -b, --benchmark    Run hardware latency benchmark\n");
            printf("  -v, --version      Show version\n");
            printf("  -h, --help         Show this help\n");
            return 0;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    g_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify_fd >= 0) {
        inotify_add_watch(g_inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = g_inotify_fd;
        epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_inotify_fd, &ev);
    }

    scan_initial_devices();

    pthread_t worker_thread;
    if (pthread_create(&worker_thread, NULL, worker_thread_func, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    struct epoll_event events[MAX_EPOLL_EVENTS];
    double last_trigger = 0.0;

    while (g_running) {
        int n = epoll_wait(g_epoll_fd, events, MAX_EPOLL_EVENTS, 2000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == g_inotify_fd) {
                handle_inotify_events();
                continue;
            }

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                struct input_event ev_buf[32];
                ssize_t bytes = read(fd, ev_buf, sizeof(ev_buf));

                if (bytes <= 0) {
                    if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }

                size_t count = (size_t)bytes / sizeof(struct input_event);
                for (size_t k = 0; k < count; k++) {
                    if (ev_buf[k].type == EV_KEY &&
                        (ev_buf[k].code == KEY_SYSRQ || ev_buf[k].code == KEY_PRINT) &&
                        ev_buf[k].value == 1) {

                        double now = get_time_ms();
                        if (now - last_trigger > DEBOUNCE_MS) {
                            last_trigger = now;

                            pthread_mutex_lock(&g_work_mutex);
                            g_work_pending = true;
                            pthread_cond_signal(&g_work_cond);
                            pthread_mutex_unlock(&g_work_mutex);
                        }
                    }
                }
            }
        }
    }

    pthread_mutex_lock(&g_work_mutex);
    g_running = 0;
    pthread_cond_broadcast(&g_work_cond);
    pthread_mutex_unlock(&g_work_mutex);

    pthread_join(worker_thread, NULL);

    if (g_user_bus) sd_bus_unref(g_user_bus);
    if (g_inotify_fd >= 0) close(g_inotify_fd);
    if (g_epoll_fd >= 0) close(g_epoll_fd);

    return 0;
}
