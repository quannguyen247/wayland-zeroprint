#define _GNU_SOURCE
#include "clipboard-wayland.h"

#include "ext-data-control-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

#define CLIPBOARD_READY_TIMEOUT_MS 2000
#define CLIPBOARD_READY_FD 3

extern char **environ;

typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *manager;
    struct ext_data_control_device_v1 *device;
    struct ext_data_control_source_v1 *source;
    int png_fd;
    bool cancelled;
} clipboard_server_t;

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    clipboard_server_t *server = data;
    if (!strcmp(interface, ext_data_control_manager_v1_interface.name)) {
        server->manager = wl_registry_bind(
            registry, name, &ext_data_control_manager_v1_interface,
            version < 1u ? version : 1u);
    } else if (!server->seat && !strcmp(interface, wl_seat_interface.name)) {
        server->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                        version < 1u ? version : 1u);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void offer_mime(void *data, struct ext_data_control_offer_v1 *offer,
                       const char *mime_type)
{
    (void)data;
    (void)offer;
    (void)mime_type;
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = offer_mime,
};

static void device_data_offer(void *data,
                              struct ext_data_control_device_v1 *device,
                              struct ext_data_control_offer_v1 *offer)
{
    (void)data;
    (void)device;
    ext_data_control_offer_v1_add_listener(offer, &offer_listener, NULL);
}

static void destroy_received_offer(struct ext_data_control_offer_v1 *offer)
{
    if (offer) ext_data_control_offer_v1_destroy(offer);
}

static void device_selection(void *data,
                             struct ext_data_control_device_v1 *device,
                             struct ext_data_control_offer_v1 *offer)
{
    (void)data;
    (void)device;
    destroy_received_offer(offer);
}

static void device_finished(void *data,
                            struct ext_data_control_device_v1 *device)
{
    clipboard_server_t *server = data;
    (void)device;
    server->cancelled = true;
}

static void device_primary_selection(
    void *data, struct ext_data_control_device_v1 *device,
    struct ext_data_control_offer_v1 *offer)
{
    (void)data;
    (void)device;
    destroy_received_offer(offer);
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = device_data_offer,
    .selection = device_selection,
    .finished = device_finished,
    .primary_selection = device_primary_selection,
};

static void source_send(void *data, struct ext_data_control_source_v1 *source,
                        const char *mime_type, int destination_fd)
{
    clipboard_server_t *server = data;
    (void)source;
    (void)mime_type;
    if (lseek(server->png_fd, 0, SEEK_SET) < 0) {
        close(destination_fd);
        return;
    }

    uint8_t buffer[128 * 1024];
    for (;;) {
        ssize_t count = read(server->png_fd, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        size_t written = 0;
        while (written < (size_t)count) {
            ssize_t result = write(destination_fd, buffer + written,
                                   (size_t)count - written);
            if (result > 0) {
                written += (size_t)result;
            } else if (result < 0 && errno == EINTR) {
                continue;
            } else {
                written = (size_t)count;
                break;
            }
        }
    }
    close(destination_fd);
}

static void source_cancelled(void *data,
                             struct ext_data_control_source_v1 *source)
{
    clipboard_server_t *server = data;
    (void)source;
    server->cancelled = true;
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

static void notify_parent(int ready_fd, uint8_t status)
{
    ssize_t ignored;
    do {
        ignored = write(ready_fd, &status, sizeof(status));
    } while (ignored < 0 && errno == EINTR);
    close(ready_fd);
}

int native_clipboard_run_server(const char *path, const char *mime_type,
                                int ready_fd)
{
    signal(SIGPIPE, SIG_IGN);
    clipboard_server_t server;
    memset(&server, 0, sizeof(server));
    server.png_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (server.png_fd < 0) {
        notify_parent(ready_fd, NATIVE_CLIPBOARD_FAILED);
        return 1;
    }

    server.display = wl_display_connect(NULL);
    if (!server.display) {
        close(server.png_fd);
        notify_parent(ready_fd, NATIVE_CLIPBOARD_UNAVAILABLE);
        return 1;
    }
    server.registry = wl_display_get_registry(server.display);
    wl_registry_add_listener(server.registry, &registry_listener, &server);
    if (wl_display_roundtrip(server.display) < 0 ||
        !server.manager || !server.seat) {
        wl_display_disconnect(server.display);
        close(server.png_fd);
        notify_parent(ready_fd, NATIVE_CLIPBOARD_UNAVAILABLE);
        return 1;
    }

    server.device = ext_data_control_manager_v1_get_data_device(
        server.manager, server.seat);
    server.source = ext_data_control_manager_v1_create_data_source(
        server.manager);
    ext_data_control_device_v1_add_listener(server.device, &device_listener,
                                            &server);
    ext_data_control_source_v1_add_listener(server.source, &source_listener,
                                            &server);
    ext_data_control_source_v1_offer(server.source, mime_type);
    ext_data_control_device_v1_set_selection(server.device, server.source);
    if (wl_display_roundtrip(server.display) < 0) {
        wl_display_disconnect(server.display);
        close(server.png_fd);
        notify_parent(ready_fd, NATIVE_CLIPBOARD_FAILED);
        return 1;
    }

    notify_parent(ready_fd, NATIVE_CLIPBOARD_STARTED);
    while (!server.cancelled && wl_display_dispatch(server.display) >= 0) {
    }

    ext_data_control_source_v1_destroy(server.source);
    ext_data_control_device_v1_destroy(server.device);
    ext_data_control_manager_v1_destroy(server.manager);
    wl_seat_destroy(server.seat);
    wl_registry_destroy(server.registry);
    wl_display_disconnect(server.display);
    close(server.png_fd);
    return 0;
}

native_clipboard_result_t native_clipboard_start(const char *path,
                                                  const char *mime_type,
                                                  pid_t *publisher_pid)
{
    int ready_pipe[2];
    if (pipe2(ready_pipe, O_CLOEXEC) < 0) return NATIVE_CLIPBOARD_FAILED;
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return NATIVE_CLIPBOARD_FAILED;
    }

    posix_spawn_file_actions_t actions;
    int spawn_result = posix_spawn_file_actions_init(&actions);
    bool actions_initialized = spawn_result == 0;
    if (spawn_result == 0) spawn_result = posix_spawn_file_actions_addclose(&actions, ready_pipe[0]);
    if (spawn_result == 0) spawn_result = posix_spawn_file_actions_adddup2(&actions, ready_pipe[1], CLIPBOARD_READY_FD);
    if (spawn_result == 0 && ready_pipe[1] != CLIPBOARD_READY_FD) {
        spawn_result = posix_spawn_file_actions_addclose(&actions, ready_pipe[1]);
    }
    if (spawn_result == 0) spawn_result = posix_spawn_file_actions_adddup2(&actions, devnull, STDIN_FILENO);
    if (spawn_result == 0) spawn_result = posix_spawn_file_actions_adddup2(&actions, devnull, STDOUT_FILENO);
    if (spawn_result == 0) spawn_result = posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);

    char *const publisher_argv[] = {
        (char *)"wayland-zeroprint",
        (char *)"--internal-clipboard-publisher",
        (char *)path,
        (char *)mime_type,
        NULL,
    };
    pid_t pid = -1;
    if (spawn_result == 0) {
        spawn_result = posix_spawn(&pid, "/proc/self/exe", &actions, NULL,
                                   publisher_argv, environ);
    }
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
    close(devnull);
    if (spawn_result != 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        errno = spawn_result;
        return NATIVE_CLIPBOARD_FAILED;
    }

    close(ready_pipe[1]);
    struct pollfd poll_fd = {
        .fd = ready_pipe[0],
        .events = POLLIN,
    };
    int ready;
    do {
        ready = poll(&poll_fd, 1, CLIPBOARD_READY_TIMEOUT_MS);
    } while (ready < 0 && errno == EINTR);
    uint8_t status = NATIVE_CLIPBOARD_FAILED;
    ssize_t count = ready > 0 ? read(ready_pipe[0], &status, sizeof(status)) : -1;
    close(ready_pipe[0]);
    if (count == (ssize_t)sizeof(status) &&
        status == NATIVE_CLIPBOARD_STARTED) {
        *publisher_pid = pid;
        return NATIVE_CLIPBOARD_STARTED;
    }

    kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    return count == (ssize_t)sizeof(status)
               ? (native_clipboard_result_t)status
               : NATIVE_CLIPBOARD_FAILED;
}

void native_clipboard_stop(pid_t *publisher_pid)
{
    if (!publisher_pid || *publisher_pid <= 0) return;
    kill(*publisher_pid, SIGTERM);
    while (waitpid(*publisher_pid, NULL, 0) < 0 && errno == EINTR) {
    }
    *publisher_pid = -1;
}
