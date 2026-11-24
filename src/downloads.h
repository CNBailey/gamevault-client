/* Download queue: resumable multi-connection downloads and PS4 installs. */
#pragma once

#include <stdint.h>
#include "util.h"

void downloads_init(void);   /* loads the queue and starts the worker */

/*
 * Queues a game from GameVault. `files` is an optional JSON array of file paths
 * (from /games/:id/files) to fetch; NULL means the whole game. `replace_path` is
 * an older copy on the console to delete once the new one is complete (updates).
 * `drive` overrides the PS5 download drive from Settings ("" = use Settings).
 * Returns 0 and writes the job id, or -1 with an error.
 */
int downloads_add(int64_t game_id, const char *files_json, const char *replace_path, const char *drive,
                  char *id_out, size_t idlen, char *err, size_t errlen);

int downloads_pause(const char *id, char *err, size_t errlen);
int downloads_resume(const char *id, char *err, size_t errlen);
int downloads_cancel(const char *id, char *err, size_t errlen);   /* stops and deletes partial data */
int downloads_remove(const char *id, char *err, size_t errlen);   /* removes a finished/cancelled job from the list */
int downloads_move(const char *id, int delta, char *err, size_t errlen); /* reorder queued jobs */
void downloads_clear_finished(void);

void downloads_json(sbuf_t *out);          /* JSON array of jobs */
int64_t downloads_reserved(const char *drive); /* bytes still to be written to a drive by queued jobs */
