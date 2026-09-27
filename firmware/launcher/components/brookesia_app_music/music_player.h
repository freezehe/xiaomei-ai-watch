/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSIC_PLAYER_STOPPED,
    MUSIC_PLAYER_BUFFERING,     // Downloading before (re)starting playback
    MUSIC_PLAYER_PLAYING,
    MUSIC_PLAYER_PAUSED,
    MUSIC_PLAYER_ERROR,         // Current track failed; `message` says why
} music_player_state_t;

typedef struct {
    music_player_state_t state;
    int index;                  // Index in the playlist, -1 if none
    int count;                  // Playlist size
    char title[128];            // Current track title (UTF-8)
    uint32_t elapsed_ms;        // Played time of the current track
    int buffer_pct;             // Ring buffer fill level, 0-100
    int volume;                 // 0-100
    uint32_t sample_rate;
    char message[64];           // Error detail
} music_player_status_t;

/**
 * @brief Start the player tasks (idempotent). Playback keeps running while the app UI is
 *        closed; the UI only sends commands and polls music_player_get_status().
 */
void music_player_init(void);

/**
 * @brief Replace the playlist and start playing `start`.
 * @param base_url  e.g. "http://host:8888/<secret>/music/"
 * @param dir       Relative directory of the files ("" or "sub/dir/")
 * @param names     File names (UTF-8, not encoded); copied
 */
void music_player_play_list(const char *base_url, const char *dir, const char *const *names, int count, int start);

void music_player_toggle_pause(void);
void music_player_next(void);
void music_player_prev(void);
void music_player_stop(void);
void music_player_set_volume(int volume);

void music_player_get_status(music_player_status_t *status);

#ifdef __cplusplus
}
#endif
