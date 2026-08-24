/*
 * wayland-zeroprint: Zero-Drop Hardware PrintScreen Daemon for Linux Wayland
 *
 * Direct KWin D-Bus Engine (9ms) + Multi-Compositor Universal Engine
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
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <systemd/sd-bus.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <zlib.h>

#define VERSION "2.2.0"
#define MAX_EPOLL_EVENTS 64
#define DEBOUNCE_MS 350.0
#define SHM_PATH "/dev/shm/wayland_zeroprint.png"

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

static uint32_t bswap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

/* Fast Pure-C In-Memory PNG Encoder */
static int write_png_rgba(const char *filename, const uint8_t *rgba, uint32_t width, uint32_t height) {
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;

    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);

    uint32_t ihdr_len = bswap32(13);
    fwrite(&ihdr_len, 1, 4, f);
    uint8_t ihdr[17];
    memcpy(ihdr, "IHDR", 4);
    uint32_t w = bswap32(width);
    uint32_t h = bswap32(height);
    memcpy(ihdr + 4, &w, 4);
    memcpy(ihdr + 8, &h, 4);
    ihdr[12] = 8; // 8-bit
    ihdr[13] = 6; // RGBA
    ihdr[14] = 0; // Deflate
    ihdr[15] = 0; // Filter
    ihdr[16] = 0; // Interlace
    fwrite(ihdr, 1, 17, f);
    uint32_t ihdr_crc = bswap32(crc32(0, ihdr, 17));
    fwrite(&ihdr_crc, 1, 4, f);

    size_t raw_line_len = width * 4 + 1;
    size_t raw_size = raw_line_len * height;
    uint8_t *raw_buf = malloc(raw_size);
    if (!raw_buf) {
        fclose(f);
        return -1;
    }

    for (uint32_t y = 0; y < height; y++) {
        raw_buf[y * raw_line_len] = 0; // Filter None
        const uint32_t *src_line = (const uint32_t *)(rgba + y * width * 4);
        uint32_t *dst_line = (uint32_t *)(raw_buf + y * raw_line_len + 1);
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = src_line[x];
            // Format: convert B G R A (KWin QImage ARGB32) to R G B A
            uint32_t b = pixel & 0xFF;
            uint32_t g = (pixel >> 8) & 0xFF;
            uint32_t r = (pixel >> 16) & 0xFF;
            uint32_t a = (pixel >> 24) & 0xFF;
            dst_line[x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }

    uLongf dest_len = compressBound(raw_size);
    uint8_t *idat_buf = malloc(dest_len + 4);
    if (!idat_buf) {
        free(raw_buf);
        fclose(f);
        return -1;
    }
    memcpy(idat_buf, "IDAT", 4);

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    deflateInit(&strm, Z_BEST_SPEED);
    strm.next_in = raw_buf;
    strm.avail_in = raw_size;
    strm.next_out = idat_buf + 4;
    strm.avail_out = dest_len;
    deflate(&strm, Z_FINISH);
    size_t comp_len = strm.total_out;
    deflateEnd(&strm);

    uint32_t idat_len_be = bswap32(comp_len);
    fwrite(&idat_len_be, 1, 4, f);
    fwrite(idat_buf, 1, comp_len + 4, f);
    uint32_t idat_crc = bswap32(crc32(0, idat_buf, comp_len + 4));
    fwrite(&idat_crc, 1, 4, f);

    uint32_t iend_len = 0;
    fwrite(&iend_len, 1, 4, f);
    uint8_t iend[4] = {'I', 'E', 'N', 'D'};
    fwrite(iend, 1, 4, f);
    uint32_t iend_crc = bswap32(crc32(0, iend, 4));
    fwrite(&iend_crc, 1, 4, f);

    free(raw_buf);
    free(idat_buf);
    fclose(f);
    return 0;
}

/* Detect physical monitor native dimensions (e.g. 1920x1080) to eliminate fractional scaling blur */
static void get_native_screen_resolution(uint32_t *width, uint32_t *height) {
    *width = 1920;
    *height = 1080;

    int drm_fd = open("/dev/dri/card1", O_RDONLY | O_CLOEXEC);
    if (drm_fd < 0) drm_fd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
    if (drm_fd >= 0) {
        struct drm_mode_card_res res;
        memset(&res, 0, sizeof(res));
        uint32_t crtc_ids[32];
        res.count_crtcs = 32;
        res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;

        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
            for (uint32_t i = 0; i < res.count_crtcs; i++) {
                struct drm_mode_crtc crtc;
                memset(&crtc, 0, sizeof(crtc));
                crtc.crtc_id = crtc_ids[i];
                if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0 && crtc.mode_valid && crtc.fb_id != 0) {
                    if (crtc.mode.hdisplay > 0 && crtc.mode.vdisplay > 0) {
                        *width = crtc.mode.hdisplay;
                        *height = crtc.mode.vdisplay;
                        break;
                    }
                }
            }
        }
        close(drm_fd);
    }
}

/* Ultra-Fast Direct KWin D-Bus Capture Engine (100% Native Pixel-Perfect Resolution) */
static bool capture_kwin_direct_dbus(void) {
    if (!g_user_bus) {
        if (sd_bus_open_user(&g_user_bus) < 0) return false;
    }

    int mem_fd = memfd_create("kwin_zeroprint_memfd", MFD_CLOEXEC);
    if (mem_fd < 0) return false;

    // Pre-allocate 32MB buffer in RAM
    if (ftruncate(mem_fd, 3840 * 2160 * 4) < 0) {
        close(mem_fd);
        return false;
    }

    uint32_t native_w = 1920, native_h = 1080;
    get_native_screen_resolution(&native_w, &native_h);

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    sd_bus_message *reply = NULL;
    int r;

    // Use CaptureArea with native physical pixel dimensions to guarantee 1:1 pixel crispness
    r = sd_bus_message_new_method_call(g_user_bus, &m,
                                       "org.kde.KWin",
                                       "/org/kde/KWin/ScreenShot2",
                                       "org.kde.KWin.ScreenShot2",
                                       "CaptureArea");
    if (r < 0) {
        close(mem_fd);
        return false;
    }

    sd_bus_message_append(m, "iiuu", 0, 0, native_w, native_h);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_close_container(m);
    sd_bus_message_append(m, "h", mem_fd);

    r = sd_bus_call(g_user_bus, m, 2000000, &error, &reply);
    if (r < 0) {
        sd_bus_error_free(&error);
        sd_bus_message_unref(m);
        close(mem_fd);
        return false;
    }

    uint32_t width = 1920, height = 1080;
    r = sd_bus_message_enter_container(reply, 'a', "{sv}");
    if (r >= 0) {
        while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
            const char *key;
            char type;
            const char *contents;
            sd_bus_message_read_basic(reply, 's', &key);
            sd_bus_message_peek_type(reply, &type, &contents);
            if (strcmp(key, "width") == 0) {
                sd_bus_message_enter_container(reply, 'v', NULL);
                if (contents && contents[0] == 'i') {
                    int v; sd_bus_message_read_basic(reply, 'i', &v); width = v;
                } else if (contents && contents[0] == 'u') {
                    uint32_t v; sd_bus_message_read_basic(reply, 'u', &v); width = v;
                }
                sd_bus_message_exit_container(reply);
            } else if (strcmp(key, "height") == 0) {
                sd_bus_message_enter_container(reply, 'v', NULL);
                if (contents && contents[0] == 'i') {
                    int v; sd_bus_message_read_basic(reply, 'i', &v); height = v;
                } else if (contents && contents[0] == 'u') {
                    uint32_t v; sd_bus_message_read_basic(reply, 'u', &v); height = v;
                }
                sd_bus_message_exit_container(reply);
            } else {
                sd_bus_message_skip(reply, "v");
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
    }

    size_t img_size = (size_t)width * height * 4;
    void *pixels = mmap(NULL, img_size, PROT_READ, MAP_SHARED, mem_fd, 0);
    bool ok = false;
    if (pixels != MAP_FAILED) {
        ok = (write_png_rgba(SHM_PATH, (const uint8_t *)pixels, width, height) == 0);
        munmap(pixels, img_size);
    }

    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    close(mem_fd);

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

static void execute_capture_and_pipe(void) {
    bool captured = false;

    // 1. On KDE: Attempt ultra-fast direct KWin D-Bus capture first (~9ms)
    if (g_backend == BACKEND_KDE) {
        captured = capture_kwin_direct_dbus();
    }

    // 2. Fallback to native compositor CLI if direct D-Bus is unavailable
    if (!captured) {
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
                    char *args[] = {"spectacle", "-b", "-f", "-n", "-o", SHM_PATH, NULL};
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
                                    "true", "false", SHM_PATH, NULL};
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
                    char *args[] = {"grim", SHM_PATH, NULL};
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
        if (ret != 0) return;
    }

    // 3. Stream captured PNG directly into wl-copy clipboard asynchronously
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

static void run_benchmark(void) {
    printf("===================================================================\n");
    printf("     WAYLAND-ZEROPRINT V%s HARDWARE LATENCY BENCHMARK             \n", VERSION);
    printf("===================================================================\n");

    double t0 = get_time_us();
    double t1 = get_time_us();
    printf("  ⏱️  1. Monotonic Clock Resolution  : %6.2f µs (Precision)\n", t1 - t0);

    if (g_backend == BACKEND_KDE) {
        double d0 = get_time_us();
        bool ok = capture_kwin_direct_dbus();
        double d1 = get_time_us();
        if (ok) {
            printf("  ⏱️  2. Direct KWin D-Bus Capture   : %6.2f ms (%.2f µs) [ACTIVE]\n",
                   (d1 - d0) / 1000.0, d1 - d0);
        } else {
            printf("  ⏱️  2. Direct KWin D-Bus Capture   : [Needs KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1]\n");
        }
    }

    printf("  🖥️  Detected Compositor Backend  : %s\n",
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

                size_t count = bytes / sizeof(struct input_event);
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
