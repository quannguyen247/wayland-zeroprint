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
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <systemd/sd-bus.h>
#include <zlib.h>

#include "clipboard-wayland.h"

#define VERSION "1.2.1"
#define MAX_EPOLL_EVENTS 64
#define MAX_TRIGGERS 32
#define DEBOUNCE_MS 350.0
#define SHM_PATH "/dev/shm/wayland_zeroprint.png"
#define SHM_TMP_PREFIX "/dev/shm/.wayland_zeroprint.tmp"
#define CAPTURE_TIMEOUT_MS 5000
#define PNG_OUTPUT_CHUNK_SIZE (128 * 1024)
#define DEFAULT_SAVE_PATH "~/Pictures/Screenshot-%Y%m%d-%H%M%S-{ms}.png"
#define MOD_SHIFT (1u << 0)
#define MOD_CTRL  (1u << 1)
#define MOD_ALT   (1u << 2)
#define MOD_META  (1u << 3)
#define ALL_MODIFIERS (MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_META)

#define QT_SHIFT_MODIFIER 0x02000000u
#define QT_CTRL_MODIFIER  0x04000000u
#define QT_ALT_MODIFIER   0x08000000u
#define QT_META_MODIFIER  0x10000000u
#define QT_KEY_PRINT      0x01000009u
#define KGLOBALACCEL_SET_PRESENT    0x02u
#define KGLOBALACCEL_NO_AUTOLOADING 0x04u

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
static sd_bus *g_shortcut_bus = NULL;
static pid_t g_native_clipboard_pid = -1;

typedef struct {
    uint16_t key_code;
    uint8_t modifiers;
    uint32_t qt_key;
    char text[64];
} trigger_t;

typedef enum {
    OUTPUT_CLIPBOARD = 0,
    OUTPUT_FILE,
    OUTPUT_BOTH
} output_mode_t;

typedef struct {
    trigger_t triggers[MAX_TRIGGERS];
    size_t trigger_count;
    bool consume_kde_shortcuts;
    bool allow_gui_fallback;
    output_mode_t output_mode;
    char save_path[PATH_MAX];
} app_config_t;

static app_config_t g_config = {
    .consume_kde_shortcuts = true,
    .allow_gui_fallback = false,
    .output_mode = OUTPUT_CLIPBOARD,
};
static unsigned int g_key_down_count[KEY_MAX + 1];

typedef struct input_device {
    int fd;
    dev_t device_number;
    bool pressed[KEY_MAX + 1];
    bool desynchronized;
    struct input_device *next;
} input_device_t;

static input_device_t *g_input_devices = NULL;
static char g_inotify_token;

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
static char g_registered_shortcut_actions[MAX_TRIGGERS][32];
static size_t g_registered_shortcut_count = 0;

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

typedef struct {
    const char *name;
    uint16_t evdev_code;
    uint32_t qt_key;
} key_name_t;

static const key_name_t g_key_names[] = {
    {"ESC", KEY_ESC, 0x01000000u},
    {"ESCAPE", KEY_ESC, 0x01000000u},
    {"TAB", KEY_TAB, 0x01000001u},
    {"BACKSPACE", KEY_BACKSPACE, 0x01000003u},
    {"ENTER", KEY_ENTER, 0x01000004u},
    {"RETURN", KEY_ENTER, 0x01000004u},
    {"SPACE", KEY_SPACE, 0x20u},
    {"INSERT", KEY_INSERT, 0x01000006u},
    {"DELETE", KEY_DELETE, 0x01000007u},
    {"HOME", KEY_HOME, 0x01000010u},
    {"END", KEY_END, 0x01000011u},
    {"LEFT", KEY_LEFT, 0x01000012u},
    {"UP", KEY_UP, 0x01000013u},
    {"RIGHT", KEY_RIGHT, 0x01000014u},
    {"DOWN", KEY_DOWN, 0x01000015u},
    {"PAGEUP", KEY_PAGEUP, 0x01000016u},
    {"PAGEDOWN", KEY_PAGEDOWN, 0x01000017u},
    {"PGUP", KEY_PAGEUP, 0x01000016u},
    {"PGDN", KEY_PAGEDOWN, 0x01000017u},
    {"PAUSE", KEY_PAUSE, 0x01000008u},
    {"SCROLLLOCK", KEY_SCROLLLOCK, 0x01000026u},
    {"CAPSLOCK", KEY_CAPSLOCK, 0x01000024u},
    {"NUMLOCK", KEY_NUMLOCK, 0x01000025u},
    {"MINUS", KEY_MINUS, '-'},
    {"EQUAL", KEY_EQUAL, '='},
    {"LEFTBRACE", KEY_LEFTBRACE, '['},
    {"RIGHTBRACE", KEY_RIGHTBRACE, ']'},
    {"SEMICOLON", KEY_SEMICOLON, ';'},
    {"APOSTROPHE", KEY_APOSTROPHE, '\''},
    {"GRAVE", KEY_GRAVE, '`'},
    {"BACKSLASH", KEY_BACKSLASH, '\\'},
    {"COMMA", KEY_COMMA, ','},
    {"DOT", KEY_DOT, '.'},
    {"PERIOD", KEY_DOT, '.'},
    {"SLASH", KEY_SLASH, '/'},
    {"KPENTER", KEY_KPENTER, 0x01000005u},
    {"KPMINUS", KEY_KPMINUS, '-'},
    {"KPPLUS", KEY_KPPLUS, '+'},
    {"KPDOT", KEY_KPDOT, '.'},
#ifdef KEY_VOLUMEUP
    {"VOLUMEUP", KEY_VOLUMEUP, 0},
    {"VOLUMEDOWN", KEY_VOLUMEDOWN, 0},
    {"MUTE", KEY_MUTE, 0},
#endif
#ifdef KEY_PLAYPAUSE
    {"PLAYPAUSE", KEY_PLAYPAUSE, 0},
    {"NEXTSONG", KEY_NEXTSONG, 0},
    {"PREVIOUSSONG", KEY_PREVIOUSSONG, 0},
    {"STOPCD", KEY_STOPCD, 0},
#endif
};

static char *trim_whitespace(char *text) {
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static bool parse_bool_value(const char *text, bool *value) {
    if (!strcasecmp(text, "true") || !strcasecmp(text, "yes") || !strcmp(text, "1")) {
        *value = true;
        return true;
    }
    if (!strcasecmp(text, "false") || !strcasecmp(text, "no") || !strcmp(text, "0")) {
        *value = false;
        return true;
    }
    return false;
}

static bool parse_output_mode(const char *text, output_mode_t *mode) {
    if (!strcasecmp(text, "clipboard")) {
        *mode = OUTPUT_CLIPBOARD;
        return true;
    }
    if (!strcasecmp(text, "file")) {
        *mode = OUTPUT_FILE;
        return true;
    }
    if (!strcasecmp(text, "both")) {
        *mode = OUTPUT_BOTH;
        return true;
    }
    return false;
}

static const char *output_mode_name(output_mode_t mode) {
    switch (mode) {
        case OUTPUT_FILE: return "file";
        case OUTPUT_BOTH: return "both";
        case OUTPUT_CLIPBOARD:
        default: return "clipboard";
    }
}

static bool lookup_key_name(const char *input, uint16_t *evdev_code, uint32_t *qt_key,
                            bool *is_print) {
    char name[64];
    size_t length = strlen(input);
    if (length >= sizeof(name)) return false;
    for (size_t i = 0; i <= length; i++) {
        name[i] = (char)toupper((unsigned char)input[i]);
    }
    const char *bare = !strncmp(name, "KEY_", 4) ? name + 4 : name;

    *is_print = false;
    if (!strcmp(bare, "PRINT") || !strcmp(bare, "PRINTSCREEN") ||
        !strcmp(bare, "PRTSC") || !strcmp(bare, "PRTSCR") ||
        !strcmp(bare, "SYSRQ")) {
        *evdev_code = KEY_SYSRQ;
        *qt_key = QT_KEY_PRINT;
        *is_print = true;
        return true;
    }

    if (bare[0] >= 'A' && bare[0] <= 'Z' && bare[1] == '\0') {
        static const uint16_t letter_codes[26] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
            KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
            KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
            KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
        };
        *evdev_code = letter_codes[bare[0] - 'A'];
        *qt_key = (uint32_t)bare[0];
        return true;
    }

    if (bare[0] >= '0' && bare[0] <= '9' && bare[1] == '\0') {
        static const uint16_t digit_codes[10] = {
            KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
            KEY_5, KEY_6, KEY_7, KEY_8, KEY_9
        };
        *evdev_code = digit_codes[bare[0] - '0'];
        *qt_key = (uint32_t)bare[0];
        return true;
    }

    if (bare[0] == 'F' && isdigit((unsigned char)bare[1])) {
        char *end = NULL;
        long number = strtol(bare + 1, &end, 10);
        if (end && *end == '\0' && number >= 1 && number <= 24) {
            static const uint16_t function_codes[24] = {
                KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
                KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
                KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
                KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24
            };
            *evdev_code = function_codes[number - 1];
            *qt_key = 0x01000030u + (uint32_t)number - 1u;
            return true;
        }
    }

    if (!strncmp(bare, "CODE_", 5) || isdigit((unsigned char)bare[0])) {
        const char *number_text = !strncmp(bare, "CODE_", 5) ? bare + 5 : bare;
        char *end = NULL;
        long number = strtol(number_text, &end, 0);
        if (end && *end == '\0' && number > 0 && number <= KEY_MAX) {
            *evdev_code = (uint16_t)number;
            *qt_key = 0;
            return true;
        }
    }

    for (size_t i = 0; i < sizeof(g_key_names) / sizeof(g_key_names[0]); i++) {
        if (!strcmp(bare, g_key_names[i].name)) {
            *evdev_code = g_key_names[i].evdev_code;
            *qt_key = g_key_names[i].qt_key;
            return true;
        }
    }
    return false;
}

static bool add_trigger(app_config_t *config, uint16_t key_code, uint8_t modifiers,
                        uint32_t qt_key, const char *text, char *error, size_t error_size) {
    for (size_t i = 0; i < config->trigger_count; i++) {
        if (config->triggers[i].key_code == key_code &&
            config->triggers[i].modifiers == modifiers) {
            return true;
        }
    }
    if (config->trigger_count >= MAX_TRIGGERS) {
        snprintf(error, error_size, "too many triggers (maximum %d)", MAX_TRIGGERS);
        return false;
    }
    trigger_t *trigger = &config->triggers[config->trigger_count++];
    trigger->key_code = key_code;
    trigger->modifiers = modifiers;
    trigger->qt_key = qt_key;
    snprintf(trigger->text, sizeof(trigger->text), "%s", text);
    return true;
}

static bool parse_trigger(app_config_t *config, const char *spec,
                          char *error, size_t error_size) {
    char copy[128];
    if (strlen(spec) >= sizeof(copy)) {
        snprintf(error, error_size, "trigger is too long: %s", spec);
        return false;
    }
    snprintf(copy, sizeof(copy), "%s", spec);

    uint8_t modifiers = 0;
    uint16_t key_code = 0;
    uint32_t qt_key = 0;
    bool key_seen = false;
    bool is_print = false;
    char *saveptr = NULL;
    for (char *part = strtok_r(copy, "+", &saveptr); part;
         part = strtok_r(NULL, "+", &saveptr)) {
        part = trim_whitespace(part);
        if (!*part) continue;
        if (!strcasecmp(part, "SHIFT")) modifiers |= MOD_SHIFT;
        else if (!strcasecmp(part, "CTRL") || !strcasecmp(part, "CONTROL")) modifiers |= MOD_CTRL;
        else if (!strcasecmp(part, "ALT")) modifiers |= MOD_ALT;
        else if (!strcasecmp(part, "META") || !strcasecmp(part, "SUPER") || !strcasecmp(part, "WIN")) modifiers |= MOD_META;
        else {
            if (key_seen) {
                snprintf(error, error_size, "trigger has more than one non-modifier key: %s", spec);
                return false;
            }
            if (!lookup_key_name(part, &key_code, &qt_key, &is_print)) {
                snprintf(error, error_size, "unknown key '%s' in trigger '%s'", part, spec);
                return false;
            }
            key_seen = true;
        }
    }
    if (!key_seen) {
        snprintf(error, error_size, "trigger needs one non-modifier key: %s", spec);
        return false;
    }

    uint32_t qt_combined = qt_key;
    if (qt_combined) {
        if (modifiers & MOD_SHIFT) qt_combined |= QT_SHIFT_MODIFIER;
        if (modifiers & MOD_CTRL) qt_combined |= QT_CTRL_MODIFIER;
        if (modifiers & MOD_ALT) qt_combined |= QT_ALT_MODIFIER;
        if (modifiers & MOD_META) qt_combined |= QT_META_MODIFIER;
    }
    if (!add_trigger(config, key_code, modifiers, qt_combined, spec, error, error_size)) {
        return false;
    }
    if (is_print && KEY_PRINT != KEY_SYSRQ) {
        if (!add_trigger(config, KEY_PRINT, modifiers, qt_combined, spec, error, error_size)) {
            return false;
        }
    }
    return true;
}

static bool parse_trigger_list(app_config_t *config, const char *list,
                               char *error, size_t error_size) {
    char *copy = strdup(list);
    if (!copy) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    config->trigger_count = 0;
    char *saveptr = NULL;
    bool ok = true;
    for (char *item = strtok_r(copy, ",", &saveptr); item;
         item = strtok_r(NULL, ",", &saveptr)) {
        item = trim_whitespace(item);
        if (*item && !parse_trigger(config, item, error, error_size)) {
            ok = false;
            break;
        }
    }
    if (ok && config->trigger_count == 0) {
        snprintf(error, error_size, "trigger list is empty");
        ok = false;
    }
    free(copy);
    return ok;
}

static void default_config_path(char *path, size_t path_size) {
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(path, path_size, "%s/wayland-zeroprint/config", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(path, path_size, "%s/.config/wayland-zeroprint/config",
             (home && *home) ? home : ".");
}

static bool load_config_file(app_config_t *config, const char *path, bool required,
                             char *error, size_t error_size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        if (!required && errno == ENOENT) return true;
        snprintf(error, error_size, "cannot open config '%.240s': %.200s",
                 path, strerror(errno));
        return false;
    }

    char line[512];
    unsigned int line_number = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        char *text = trim_whitespace(line);
        if (!*text || *text == '#' || *text == ';') continue;
        char *equals = strchr(text, '=');
        if (!equals) {
            snprintf(error, error_size, "%.240s:%u: expected key=value", path, line_number);
            ok = false;
            break;
        }
        *equals = '\0';
        char *key = trim_whitespace(text);
        char *value = trim_whitespace(equals + 1);
        if (!strcasecmp(key, "triggers") || !strcasecmp(key, "trigger")) {
            if (!parse_trigger_list(config, value, error, error_size)) {
                char detail[256];
                snprintf(detail, sizeof(detail), "%.240s", error);
                snprintf(error, error_size, "%.200s:%u: %.240s",
                         path, line_number, detail);
                ok = false;
                break;
            }
        } else if (!strcasecmp(key, "consume_kde_shortcuts")) {
            if (!parse_bool_value(value, &config->consume_kde_shortcuts)) {
                snprintf(error, error_size, "%.200s:%u: invalid boolean '%.200s'",
                         path, line_number, value);
                ok = false;
                break;
            }
        } else if (!strcasecmp(key, "allow_gui_fallback")) {
            if (!parse_bool_value(value, &config->allow_gui_fallback)) {
                snprintf(error, error_size, "%.200s:%u: invalid boolean '%.200s'",
                         path, line_number, value);
                ok = false;
                break;
            }
        } else if (!strcasecmp(key, "output_mode")) {
            if (!parse_output_mode(value, &config->output_mode)) {
                snprintf(error, error_size,
                         "%.240s:%u: output_mode must be clipboard, file, or both",
                         path, line_number);
                ok = false;
                break;
            }
        } else if (!strcasecmp(key, "save_path")) {
            if (!*value || strlen(value) >= sizeof(config->save_path)) {
                snprintf(error, error_size, "%.240s:%u: invalid save_path", path, line_number);
                ok = false;
                break;
            }
            snprintf(config->save_path, sizeof(config->save_path), "%s", value);
        } else {
            snprintf(error, error_size, "%.200s:%u: unknown option '%.200s'",
                     path, line_number, key);
            ok = false;
            break;
        }
    }
    if (ferror(file) && ok) {
        snprintf(error, error_size, "cannot read config '%.240s': %.200s",
                 path, strerror(errno));
        ok = false;
    }
    fclose(file);
    return ok;
}

static bool initialize_config(app_config_t *config, const char *path, bool path_is_explicit,
                              char *error, size_t error_size) {
    memset(config, 0, sizeof(*config));
    config->consume_kde_shortcuts = true;
    config->allow_gui_fallback = false;
    config->output_mode = OUTPUT_CLIPBOARD;
    snprintf(config->save_path, sizeof(config->save_path), "%s", DEFAULT_SAVE_PATH);
    if (!parse_trigger_list(config, "PRINT", error, error_size)) return false;
    if (!load_config_file(config, path, path_is_explicit, error, error_size)) return false;

    const char *environment_triggers = getenv("WAYLAND_ZEROPRINT_TRIGGERS");
    if (environment_triggers && *environment_triggers &&
        !parse_trigger_list(config, environment_triggers, error, error_size)) {
        char detail[256];
        snprintf(detail, sizeof(detail), "%.240s", error);
        snprintf(error, error_size, "WAYLAND_ZEROPRINT_TRIGGERS: %s", detail);
        return false;
    }

    const char *environment_output = getenv("WAYLAND_ZEROPRINT_OUTPUT");
    if (environment_output && *environment_output &&
        !parse_output_mode(environment_output, &config->output_mode)) {
        snprintf(error, error_size,
                 "WAYLAND_ZEROPRINT_OUTPUT must be clipboard, file, or both");
        return false;
    }
    const char *environment_save_path = getenv("WAYLAND_ZEROPRINT_SAVE_PATH");
    if (environment_save_path && *environment_save_path) {
        if (strlen(environment_save_path) >= sizeof(config->save_path)) {
            snprintf(error, error_size, "WAYLAND_ZEROPRINT_SAVE_PATH is too long");
            return false;
        }
        snprintf(config->save_path, sizeof(config->save_path), "%s",
                 environment_save_path);
    }
    return true;
}

static uint8_t current_modifier_mask(void) {
    uint8_t modifiers = 0;
    if (g_key_down_count[KEY_LEFTSHIFT] || g_key_down_count[KEY_RIGHTSHIFT]) modifiers |= MOD_SHIFT;
    if (g_key_down_count[KEY_LEFTCTRL] || g_key_down_count[KEY_RIGHTCTRL]) modifiers |= MOD_CTRL;
    if (g_key_down_count[KEY_LEFTALT] || g_key_down_count[KEY_RIGHTALT]) modifiers |= MOD_ALT;
    if (g_key_down_count[KEY_LEFTMETA] || g_key_down_count[KEY_RIGHTMETA]) modifiers |= MOD_META;
    return modifiers;
}

static bool trigger_matches(uint16_t key_code, uint8_t modifiers) {
    for (size_t i = 0; i < g_config.trigger_count; i++) {
        if (g_config.triggers[i].key_code == key_code &&
            g_config.triggers[i].modifiers == modifiers) {
            return true;
        }
    }
    return false;
}

static void print_active_config(const app_config_t *config, const char *path) {
    printf("Config file: %s\n", path);
    printf("Triggers:\n");
    for (size_t i = 0; i < config->trigger_count; i++) {
        printf("  %s (evdev=%u%s)\n", config->triggers[i].text,
               config->triggers[i].key_code,
               config->triggers[i].qt_key ? ", KDE-consumable" : ", passive-only");
    }
    printf("Consume KDE shortcuts: %s\n", config->consume_kde_shortcuts ? "true" : "false");
    printf("Allow GUI fallback: %s\n", config->allow_gui_fallback ? "true" : "false");
    printf("Output mode: %s\n", output_mode_name(config->output_mode));
    printf("Save path: %s\n", config->save_path);
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

static bool append_string_array(sd_bus_message *message, const char *const *values,
                                size_t count) {
    if (sd_bus_message_open_container(message, 'a', "s") < 0) return false;
    for (size_t i = 0; i < count; i++) {
        if (sd_bus_message_append(message, "s", values[i]) < 0) return false;
    }
    return sd_bus_message_close_container(message) >= 0;
}

static int call_kglobalaccel_with_action(const char *method, const char *action_name,
                                         sd_bus_message **reply_out) {
    const char *action_id[] = {
        "wayland-zeroprint", action_name,
        "Wayland Zeroprint", "Capture screenshot"
    };
    sd_bus_message *message = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int result = sd_bus_message_new_method_call(g_shortcut_bus, &message,
                                                "org.kde.kglobalaccel",
                                                "/kglobalaccel",
                                                "org.kde.KGlobalAccel",
                                                method);
    if (result >= 0 && !append_string_array(message, action_id, 4)) result = -EINVAL;
    if (result >= 0) {
        result = sd_bus_call(g_shortcut_bus, message, 2000000, &error, reply_out);
    }
    if (result < 0 && strcmp(method, "unRegister") != 0) {
        fprintf(stderr, "wayland-zeroprint: KGlobalAccel %s failed: %s\n",
                method, error.message ? error.message : strerror(-result));
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(message);
    return result;
}

static bool set_kglobalaccel_shortcut(const char *action_name, uint32_t qt_key) {
    const char *action_id[] = {
        "wayland-zeroprint", action_name,
        "Wayland Zeroprint", "Capture screenshot"
    };
    sd_bus_message *message = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    bool assigned = false;

    int result = sd_bus_message_new_method_call(g_shortcut_bus, &message,
                                                "org.kde.kglobalaccel",
                                                "/kglobalaccel",
                                                "org.kde.KGlobalAccel",
                                                "setShortcut");
    if (result >= 0 && !append_string_array(message, action_id, 4)) result = -EINVAL;
    if (result >= 0 && sd_bus_message_open_container(message, 'a', "i") < 0) result = -EINVAL;
    if (result >= 0 && sd_bus_message_append(message, "i", (int32_t)qt_key) < 0) result = -EINVAL;
    if (result >= 0 && sd_bus_message_close_container(message) < 0) result = -EINVAL;
    if (result >= 0 &&
        sd_bus_message_append(message, "u",
                              KGLOBALACCEL_SET_PRESENT |
                              KGLOBALACCEL_NO_AUTOLOADING) < 0) {
        result = -EINVAL;
    }
    if (result >= 0) result = sd_bus_call(g_shortcut_bus, message, 2000000, &error, &reply);
    if (result >= 0 && sd_bus_message_enter_container(reply, 'a', "i") >= 0) {
        int32_t returned_key = 0;
        while (sd_bus_message_read(reply, "i", &returned_key) > 0) {
            if ((uint32_t)returned_key == qt_key) assigned = true;
        }
    }
    if (result < 0) {
        fprintf(stderr, "wayland-zeroprint: KGlobalAccel setShortcut failed: %s\n",
                error.message ? error.message : strerror(-result));
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(message);
    sd_bus_message_unref(reply);
    return assigned;
}

static void unregister_kde_shortcuts(void) {
    if (!g_shortcut_bus) return;
    for (size_t i = 0; i < g_registered_shortcut_count; i++) {
        sd_bus_message *reply = NULL;
        call_kglobalaccel_with_action("unRegister", g_registered_shortcut_actions[i], &reply);
        sd_bus_message_unref(reply);
    }
    g_registered_shortcut_count = 0;
}

static void register_kde_shortcuts(void) {
    if (g_backend != BACKEND_KDE || !g_config.consume_kde_shortcuts) return;
    if (sd_bus_open_user(&g_shortcut_bus) < 0) {
        fprintf(stderr, "wayland-zeroprint: cannot connect to KGlobalAccel; triggers remain passive\n");
        return;
    }

    uint32_t registered_keys[MAX_TRIGGERS] = {0};
    size_t registered_key_count = 0;
    for (size_t i = 0; i < g_config.trigger_count; i++) {
        const uint32_t qt_key = g_config.triggers[i].qt_key;
        if (!qt_key) {
            fprintf(stderr,
                    "wayland-zeroprint: trigger '%s' has no Qt mapping; KDE cannot consume it\n",
                    g_config.triggers[i].text);
            continue;
        }
        bool duplicate = false;
        for (size_t k = 0; k < registered_key_count; k++) {
            if (registered_keys[k] == qt_key) duplicate = true;
        }
        if (duplicate) continue;

        char action_name[32];
        snprintf(action_name, sizeof(action_name), "capture_%zu", registered_key_count);
        sd_bus_message *reply = NULL;
        call_kglobalaccel_with_action("unRegister", action_name, &reply);
        sd_bus_message_unref(reply);
        reply = NULL;
        if (call_kglobalaccel_with_action("doRegister", action_name, &reply) < 0) {
            sd_bus_message_unref(reply);
            continue;
        }
        sd_bus_message_unref(reply);
        if (!set_kglobalaccel_shortcut(action_name, qt_key)) {
            fprintf(stderr,
                    "wayland-zeroprint: KDE shortcut conflict for '%s'; not consuming it\n",
                    g_config.triggers[i].text);
            reply = NULL;
            call_kglobalaccel_with_action("unRegister", action_name, &reply);
            sd_bus_message_unref(reply);
            continue;
        }
        snprintf(g_registered_shortcut_actions[g_registered_shortcut_count],
                 sizeof(g_registered_shortcut_actions[0]), "%s", action_name);
        g_registered_shortcut_count++;
        registered_keys[registered_key_count++] = qt_key;
    }
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

static bool replace_millisecond_placeholder(const char *source, char *destination,
                                             size_t destination_size,
                                             unsigned int milliseconds) {
    const char *placeholder = "{ms}";
    if (milliseconds > 999u) return false;
    char millisecond_text[4] = {
        (char)('0' + milliseconds / 100u),
        (char)('0' + (milliseconds / 10u) % 10u),
        (char)('0' + milliseconds % 10u),
        '\0'
    };
    size_t used = 0;
    while (*source) {
        const char *match = strstr(source, placeholder);
        size_t chunk = match ? (size_t)(match - source) : strlen(source);
        if (chunk >= destination_size - used) return false;
        memcpy(destination + used, source, chunk);
        used += chunk;
        source += chunk;
        if (!match) break;
        if (3 >= destination_size - used) return false;
        memcpy(destination + used, millisecond_text, 3);
        used += 3;
        source += strlen(placeholder);
    }
    destination[used] = '\0';
    return true;
}

static bool build_save_path(char *destination, size_t destination_size) {
    const char *configured = g_config.save_path;
    const char *home = getenv("HOME");
    char absolute_template[PATH_MAX];
    if (!strncmp(configured, "~/", 2)) {
        if (!home || !*home ||
            snprintf(absolute_template, sizeof(absolute_template), "%s/%s",
                     home, configured + 2) >= (int)sizeof(absolute_template)) {
            return false;
        }
    } else if (configured[0] == '/') {
        if (snprintf(absolute_template, sizeof(absolute_template), "%s", configured) >=
            (int)sizeof(absolute_template)) {
            return false;
        }
    } else {
        if (!home || !*home ||
            snprintf(absolute_template, sizeof(absolute_template), "%s/%s",
                     home, configured) >= (int)sizeof(absolute_template)) {
            return false;
        }
    }

    struct stat path_stat;
    size_t length = strlen(absolute_template);
    if ((length > 0 && absolute_template[length - 1] == '/') ||
        (stat(absolute_template, &path_stat) == 0 && S_ISDIR(path_stat.st_mode))) {
        const char *separator = (length > 0 && absolute_template[length - 1] == '/') ? "" : "/";
        if (snprintf(absolute_template + length, sizeof(absolute_template) - length,
                     "%sScreenshot-%%Y%%m%%d-%%H%%M%%S-{ms}.png", separator) >=
            (int)(sizeof(absolute_template) - length)) {
            return false;
        }
    }

    struct timespec realtime;
    if (clock_gettime(CLOCK_REALTIME, &realtime) < 0) return false;
    char with_milliseconds[PATH_MAX];
    if (!replace_millisecond_placeholder(absolute_template, with_milliseconds,
                                         sizeof(with_milliseconds),
                                         (unsigned int)(realtime.tv_nsec / 1000000L))) {
        return false;
    }
    struct tm local_time;
    if (!localtime_r(&realtime.tv_sec, &local_time)) return false;
    return strftime(destination, destination_size, with_milliseconds, &local_time) > 0;
}

static bool ensure_parent_directories(const char *file_path) {
    char path[PATH_MAX];
    if (strlen(file_path) >= sizeof(path)) return false;
    snprintf(path, sizeof(path), "%s", file_path);
    char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) return true;
    *last_slash = '\0';
    for (char *cursor = path + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(path, 0700) < 0 && errno != EEXIST) return false;
        *cursor = '/';
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static bool copy_png_atomically(const char *source_path, const char *destination_path) {
    if (!ensure_parent_directories(destination_path)) return false;
    char temporary_path[PATH_MAX];
    if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.%ld",
                 destination_path, (long)getpid()) >= (int)sizeof(temporary_path)) {
        errno = ENAMETOOLONG;
        return false;
    }

    int source_fd = open(source_path, O_RDONLY | O_CLOEXEC);
    if (source_fd < 0) return false;
    unlink(temporary_path);
    int destination_fd = open(temporary_path,
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (destination_fd < 0) {
        close(source_fd);
        return false;
    }

    bool ok = true;
    int saved_errno = 0;
    uint8_t buffer[128 * 1024];
    while (ok) {
        ssize_t count = read(source_fd, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            saved_errno = errno;
            ok = false;
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
                saved_errno = result < 0 ? errno : EIO;
                ok = false;
                break;
            }
        }
    }
    if (close(source_fd) < 0 && ok) {
        saved_errno = errno;
        ok = false;
    }
    if (close(destination_fd) < 0 && ok) {
        saved_errno = errno;
        ok = false;
    }
    if (ok && rename(temporary_path, destination_path) < 0) {
        saved_errno = errno;
        ok = false;
    }
    if (!ok) unlink(temporary_path);
    if (!ok) errno = saved_errno ? saved_errno : EIO;
    return ok;
}

static pid_t start_wl_copy_publish(void) {
    int fd = open(SHM_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    pid_t copy_pid = fork();
    if (copy_pid == 0) {
        dup2(fd, STDIN_FILENO);
        close(fd);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        char *args[] = {"wl-copy", "-t", "image/png", NULL};
        execvp("wl-copy", args);
        _exit(127);
    }
    close(fd);
    return copy_pid;
}

static bool publish_clipboard(void) {
    pid_t new_publisher_pid = -1;
    native_clipboard_result_t native_result =
        native_clipboard_start(SHM_PATH, "image/png", &new_publisher_pid);
    if (native_result == NATIVE_CLIPBOARD_STARTED) {
        pid_t previous_publisher_pid = g_native_clipboard_pid;
        g_native_clipboard_pid = new_publisher_pid;
        native_clipboard_stop(&previous_publisher_pid);
        return true;
    }

    /*
     * wl-copy falls back to a transparent xdg_toplevel when the compositor
     * lacks a data-control protocol. KWin treats that temporary toplevel as a
     * normal window and exits Peek at Desktop, so never use that fallback on
     * KDE. ext-data-control-v1 is available in Plasma 6.6 and newer.
     */
    if (g_backend == BACKEND_KDE) {
        fprintf(stderr,
                native_result == NATIVE_CLIPBOARD_UNAVAILABLE
                    ? "wayland-zeroprint: ext-data-control-v1 is unavailable; refusing focus-changing wl-copy fallback on KDE\n"
                    : "wayland-zeroprint: native Wayland clipboard publication failed\n");
        return false;
    }

    pid_t copy_pid = start_wl_copy_publish();
    if (copy_pid < 0) {
        fprintf(stderr, "wayland-zeroprint: cannot start clipboard publisher: %s\n",
                strerror(errno));
        return false;
    }
    int copy_status = 0;
    if (waitpid(copy_pid, &copy_status, 0) < 0 ||
        !WIFEXITED(copy_status) || WEXITSTATUS(copy_status) != 0) {
        fprintf(stderr, "wayland-zeroprint: wl-copy failed; clipboard was not updated\n");
        return false;
    }
    return true;
}

static bool execute_capture_and_pipe(void) {
    bool captured = false;
    char temp_path[128];
    make_temp_path(temp_path, sizeof(temp_path));

    /* Prefer the direct KWin raw-frame path on KDE. */
    if (g_backend == BACKEND_KDE) {
        captured = capture_kwin_direct_dbus();
    }

    if (!captured && g_backend == BACKEND_KDE && !g_config.allow_gui_fallback) {
        fprintf(stderr,
                "wayland-zeroprint: direct KWin capture failed; GUI fallback is disabled\n");
        return false;
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
            return false;
        }
    }

    const bool wants_clipboard = g_config.output_mode != OUTPUT_FILE;
    const bool wants_file = g_config.output_mode != OUTPUT_CLIPBOARD;
    bool output_ok = true;

    if (wants_clipboard && !publish_clipboard()) output_ok = false;

    if (wants_file) {
        char save_path[PATH_MAX];
        if (!build_save_path(save_path, sizeof(save_path))) {
            fprintf(stderr, "wayland-zeroprint: invalid or expanded save_path is too long\n");
            output_ok = false;
        } else if (!copy_png_atomically(SHM_PATH, save_path)) {
            fprintf(stderr, "wayland-zeroprint: cannot save '%s': %s\n",
                    save_path, strerror(errno));
            output_ok = false;
        }
    }

    return output_ok;
}

static void queue_capture(void) {
    pthread_mutex_lock(&g_work_mutex);
    g_work_pending = true;
    pthread_cond_signal(&g_work_cond);
    pthread_mutex_unlock(&g_work_mutex);
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

        (void)execute_capture_and_pipe();
    }
    return NULL;
}

static bool bit_is_set(const unsigned long *bits, unsigned int bit) {
    const unsigned int bits_per_word = sizeof(unsigned long) * 8u;
    return (bits[bit / bits_per_word] & (1ul << (bit % bits_per_word))) != 0;
}

static bool device_has_relevant_keys(const unsigned long *key_bits) {
    static const uint16_t modifiers[] = {
        KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
        KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA
    };
    for (size_t i = 0; i < g_config.trigger_count; i++) {
        if (bit_is_set(key_bits, g_config.triggers[i].key_code)) return true;
    }
    for (size_t i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); i++) {
        if (bit_is_set(key_bits, modifiers[i])) return true;
    }
    return false;
}

static void update_device_key(input_device_t *device, uint16_t code, bool pressed) {
    if (code > KEY_MAX || device->pressed[code] == pressed) return;
    device->pressed[code] = pressed;
    if (pressed) {
        if (g_key_down_count[code] < UINT_MAX) g_key_down_count[code]++;
    } else if (g_key_down_count[code] > 0) {
        g_key_down_count[code]--;
    }
}

static bool resynchronize_device(input_device_t *device) {
    enum { BITS_PER_WORD = sizeof(unsigned long) * 8u };
    unsigned long key_state[(KEY_MAX + BITS_PER_WORD) / BITS_PER_WORD];
    memset(key_state, 0, sizeof(key_state));
    if (ioctl(device->fd, EVIOCGKEY(sizeof(key_state)), key_state) < 0) return false;
    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        update_device_key(device, (uint16_t)code, bit_is_set(key_state, code));
    }
    device->desynchronized = false;
    return true;
}

static void remove_device(input_device_t *device) {
    input_device_t **link = &g_input_devices;
    while (*link && *link != device) link = &(*link)->next;
    if (*link == device) *link = device->next;
    for (unsigned int code = 0; code <= KEY_MAX; code++) {
        if (device->pressed[code] && g_key_down_count[code] > 0) {
            g_key_down_count[code]--;
        }
    }
    epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, device->fd, NULL);
    close(device->fd);
    free(device);
}

static void add_device(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;

    struct stat stat_buffer;
    if (fstat(fd, &stat_buffer) < 0) {
        close(fd);
        return;
    }
    for (input_device_t *existing = g_input_devices; existing; existing = existing->next) {
        if (existing->device_number == stat_buffer.st_rdev) {
            close(fd);
            return;
        }
    }

    enum { BITS_PER_WORD = sizeof(unsigned long) * 8u };
    unsigned long event_bits[(EV_MAX + BITS_PER_WORD) / BITS_PER_WORD];
    unsigned long key_bits[(KEY_MAX + BITS_PER_WORD) / BITS_PER_WORD];
    memset(event_bits, 0, sizeof(event_bits));
    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits) < 0 ||
        !bit_is_set(event_bits, EV_KEY) ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
        !device_has_relevant_keys(key_bits)) {
        close(fd);
        return;
    }

    input_device_t *device = calloc(1, sizeof(*device));
    if (!device) {
        close(fd);
        return;
    }
    device->fd = fd;
    device->device_number = stat_buffer.st_rdev;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = device;

    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(device);
        close(fd);
        return;
    }
    device->next = g_input_devices;
    g_input_devices = device;
    resynchronize_device(device);
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
           (g_backend == BACKEND_KDE && g_config.allow_gui_fallback) ?
               "KDE Plasma 6 (Direct D-Bus + opt-in Spectacle fallback)" :
           (g_backend == BACKEND_KDE) ? "KDE Plasma 6 (Direct D-Bus; GUI fallback disabled)" :
           (g_backend == BACKEND_GNOME) ? "GNOME Shell (gdbus)" : "wlroots (grim)");
    printf("===================================================================\n");
}

static int run_self_tests(void) {
    app_config_t test_config = {0};
    char error[256] = {0};
    int failures = 0;
#define CHECK_TEST(condition, message) \
    do { if (!(condition)) { fprintf(stderr, "SELF-TEST FAIL: %s\n", message); failures++; } } while (0)

    CHECK_TEST(parse_trigger_list(&test_config, "PRINT", error, sizeof(error)),
               "PRINT parses");
    CHECK_TEST(test_config.trigger_count == (KEY_PRINT == KEY_SYSRQ ? 1u : 2u),
               "PRINT expands to supported evdev aliases");
    CHECK_TEST(test_config.triggers[0].qt_key == QT_KEY_PRINT,
               "PRINT has the Qt mapping used by KGlobalAccel");

    CHECK_TEST(parse_trigger_list(&test_config, "CTRL+ALT+P,F12,F13,B", error, sizeof(error)),
               "mixed chord list parses");
    CHECK_TEST(test_config.trigger_count == 4, "mixed chord count");
    CHECK_TEST(test_config.triggers[0].key_code == KEY_P &&
               test_config.triggers[0].modifiers == (MOD_CTRL | MOD_ALT),
               "CTRL+ALT+P maps to physical KEY_P with exact modifiers");
    CHECK_TEST(test_config.triggers[1].key_code == KEY_F12,
               "F12 maps across the evdev function-key gap");
    CHECK_TEST(test_config.triggers[2].key_code == KEY_F13,
               "F13 maps across the evdev function-key gap");
    CHECK_TEST(test_config.triggers[3].key_code == KEY_B,
               "letters use the non-contiguous evdev mapping");

    CHECK_TEST(parse_trigger_list(&test_config, "CODE_200", error, sizeof(error)) &&
               test_config.triggers[0].qt_key == 0,
               "numeric evdev fallback parses as passive-only");
    CHECK_TEST(!parse_trigger_list(&test_config, "CTRL+NO_SUCH_KEY", error, sizeof(error)),
               "unknown key is rejected");
    CHECK_TEST(!parse_trigger_list(&test_config, "CTRL+ALT", error, sizeof(error)),
               "modifier-only trigger is rejected");

    memset(&g_config, 0, sizeof(g_config));
    CHECK_TEST(parse_trigger_list(&g_config, "CTRL+P", error, sizeof(error)),
               "exact-match fixture parses");
    CHECK_TEST(trigger_matches(KEY_P, MOD_CTRL), "exact modifier chord matches");
    CHECK_TEST(!trigger_matches(KEY_P, 0), "missing modifier does not match");
    CHECK_TEST(!trigger_matches(KEY_P, MOD_CTRL | MOD_SHIFT),
               "extra modifier does not match");

    output_mode_t output_mode = OUTPUT_CLIPBOARD;
    CHECK_TEST(parse_output_mode("clipboard", &output_mode) &&
               output_mode == OUTPUT_CLIPBOARD, "clipboard output mode parses");
    CHECK_TEST(parse_output_mode("file", &output_mode) &&
               output_mode == OUTPUT_FILE, "file output mode parses");
    CHECK_TEST(parse_output_mode("both", &output_mode) &&
               output_mode == OUTPUT_BOTH, "both output mode parses");
    CHECK_TEST(!parse_output_mode("printer", &output_mode),
               "unknown output mode is rejected");
    g_config.output_mode = OUTPUT_FILE;
    snprintf(g_config.save_path, sizeof(g_config.save_path),
             "%s", "/tmp/wayland-zeroprint-self-test/{ms}-%Y.png");
    char expanded_path[PATH_MAX];
    CHECK_TEST(build_save_path(expanded_path, sizeof(expanded_path)) &&
               strstr(expanded_path, "/tmp/wayland-zeroprint-self-test/") == expanded_path &&
               strstr(expanded_path, "{ms}") == NULL &&
               strstr(expanded_path, "%Y") == NULL,
               "save path expands milliseconds and strftime placeholders");

#undef CHECK_TEST
    if (failures == 0) {
        printf("All wayland-zeroprint self-tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d self-test(s) failed.\n", failures);
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--internal-clipboard-publisher")) {
        return native_clipboard_run_server(argv[2], argv[3], 3);
    }

    char config_path[PATH_MAX];
    char config_error[512] = {0};
    bool config_path_is_explicit = false;
    default_config_path(config_path, sizeof(config_path));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config")) {
            if (++i >= argc) {
                fprintf(stderr, "--config requires a path\n");
                return 2;
            }
            snprintf(config_path, sizeof(config_path), "%s", argv[i]);
            config_path_is_explicit = true;
        }
    }
    if (!initialize_config(&g_config, config_path, config_path_is_explicit,
                           config_error, sizeof(config_error))) {
        fprintf(stderr, "wayland-zeroprint: %s\n", config_error);
        return 2;
    }

    g_backend = detect_backend();

    bool cli_trigger_override = false;
    bool want_benchmark = false;
    bool want_capture = false;
    bool want_version = false;
    bool want_print_config = false;
    bool want_help = false;
    bool want_self_test = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--benchmark") == 0 || strcmp(argv[i], "-b") == 0) {
            want_benchmark = true;
        } else if (strcmp(argv[i], "--capture") == 0) {
            want_capture = true;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            want_version = true;
        } else if (strcmp(argv[i], "--print-config") == 0) {
            want_print_config = true;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            want_self_test = true;
        } else if (strcmp(argv[i], "--config") == 0) {
            i++;
        } else if (strcmp(argv[i], "--trigger") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--trigger requires a chord\n");
                return 2;
            }
            if (!cli_trigger_override) {
                g_config.trigger_count = 0;
                cli_trigger_override = true;
            }
            if (!parse_trigger(&g_config, argv[i], config_error, sizeof(config_error))) {
                fprintf(stderr, "wayland-zeroprint: %s\n", config_error);
                return 2;
            }
        } else if (strcmp(argv[i], "--output") == 0) {
            if (++i >= argc || !parse_output_mode(argv[i], &g_config.output_mode)) {
                fprintf(stderr, "--output requires clipboard, file, or both\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--save-path") == 0) {
            if (++i >= argc || !*argv[i] || strlen(argv[i]) >= sizeof(g_config.save_path)) {
                fprintf(stderr, "--save-path requires a valid path/template\n");
                return 2;
            }
            snprintf(g_config.save_path, sizeof(g_config.save_path), "%s", argv[i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            want_help = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        }
    }
    if (want_help) {
        printf("Usage: wayland-zeroprint [options]\n");
        printf("Options:\n");
        printf("  -b, --benchmark    Run capture benchmark\n");
        printf("      --capture      Capture once, publish configured outputs, and exit\n");
        printf("      --config FILE  Read an explicit config file\n");
        printf("      --trigger KEY  Override trigger; may be repeated\n");
        printf("      --output MODE  clipboard, file, or both\n");
        printf("      --save-path P  File path, directory, or strftime template\n");
        printf("      --print-config Validate and show the active configuration\n");
        printf("      --self-test    Run parser and matching unit tests\n");
        printf("  -v, --version      Show version\n");
        printf("  -h, --help         Show this help\n");
        return 0;
    }
    if (want_version) {
        printf("wayland-zeroprint version %s (Native C + Direct KWin Engine)\n", VERSION);
        return 0;
    }
    if (want_self_test) return run_self_tests();
    if (want_print_config) {
        print_active_config(&g_config, config_path);
        return 0;
    }
    if (want_benchmark) {
        run_benchmark();
        return 0;
    }
    if (want_capture) return execute_capture_and_pipe() ? 0 : 1;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    register_kde_shortcuts();

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
        ev.data.ptr = &g_inotify_token;
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
            if (events[i].data.ptr == &g_inotify_token) {
                handle_inotify_events();
                continue;
            }

            input_device_t *device = events[i].data.ptr;
            if (!device) continue;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                remove_device(device);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                struct input_event ev_buf[32];
                ssize_t bytes = read(device->fd, ev_buf, sizeof(ev_buf));

                if (bytes <= 0) {
                    if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    remove_device(device);
                    continue;
                }

                size_t count = (size_t)bytes / sizeof(struct input_event);
                for (size_t k = 0; k < count; k++) {
                    if (ev_buf[k].type == EV_SYN && ev_buf[k].code == SYN_DROPPED) {
                        device->desynchronized = true;
                        continue;
                    }
                    if (device->desynchronized) {
                        if (ev_buf[k].type == EV_SYN && ev_buf[k].code == SYN_REPORT) {
                            resynchronize_device(device);
                        }
                        continue;
                    }
                    if (ev_buf[k].type != EV_KEY || ev_buf[k].code > KEY_MAX) {
                        continue;
                    }
                    if (ev_buf[k].value == 1) {
                        update_device_key(device, ev_buf[k].code, true);
                    } else if (ev_buf[k].value == 0) {
                        update_device_key(device, ev_buf[k].code, false);
                    }

                    if (ev_buf[k].value == 1 &&
                        trigger_matches(ev_buf[k].code, current_modifier_mask())) {

                        double now = get_time_ms();
                        if (now - last_trigger > DEBOUNCE_MS) {
                            last_trigger = now;
                            queue_capture();
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

    unregister_kde_shortcuts();
    native_clipboard_stop(&g_native_clipboard_pid);
    if (g_shortcut_bus) sd_bus_unref(g_shortcut_bus);
    if (g_user_bus) sd_bus_unref(g_user_bus);
    while (g_input_devices) remove_device(g_input_devices);
    if (g_inotify_fd >= 0) close(g_inotify_fd);
    if (g_epoll_fd >= 0) close(g_epoll_fd);

    return 0;
}
