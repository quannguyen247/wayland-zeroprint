#ifndef WAYLAND_ZEROPRINT_CLIPBOARD_WAYLAND_H
#define WAYLAND_ZEROPRINT_CLIPBOARD_WAYLAND_H

#include <sys/types.h>

typedef enum {
    NATIVE_CLIPBOARD_STARTED = 0,
    NATIVE_CLIPBOARD_UNAVAILABLE,
    NATIVE_CLIPBOARD_FAILED
} native_clipboard_result_t;

native_clipboard_result_t native_clipboard_start(const char *path,
                                                  const char *mime_type,
                                                  pid_t *publisher_pid);
int native_clipboard_run_server(const char *path, const char *mime_type,
                                int ready_fd);
void native_clipboard_stop(pid_t *publisher_pid);

#endif
