/* Everything that differs between the PS5 payload and the Linux test build. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called once at start-up. */
void platform_init(void);
/* True when a drive is really mounted at this console path (not just an empty folder). */
bool platform_is_mounted(const char *console_path);

/* Directory for settings, the queue and resume data. */
const char *platform_data_dir(void);

/*
 * Maps a console path ("/data/homebrew", "/mnt/usb0") to a real path.
 * On the PS5 this is the identity; the Linux build prefixes $GV_ROOT so tests
 * can fake a console's file system.
 */
void platform_path(const char *console_path, char *out, size_t outlen);

/* System firmware, e.g. "9.60", or "" when unknown. */
void platform_firmware(char *out, size_t outlen);

/* True while a game (a "big app") is running in the foreground. */
bool platform_game_running(void);

/* Pop-up notification on the console. */
void platform_notify(const char *title, const char *body);

/* Opens the console's web browser at a URL (the TV interface). */
int platform_open_browser(const char *url);

/* Hands a PS4 .pkg URL to the console's package installer. 0 on success. */
int platform_install_pkg(const char *url, const char *content_id, const char *name, const char *icon_url, char *err, size_t errlen);
