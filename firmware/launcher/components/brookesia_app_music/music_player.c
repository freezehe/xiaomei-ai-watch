/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "music_player.h"

#include <stdlib.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_codec_dev.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music_net.h"

#define TAG "MusicPlayer"

// ~16 s of a 128 kbps MP3: rides out phone-hotspot hiccups
#define RB_SIZE             (256 * 1024)
#define PREBUFFER_BYTES     (32 * 1024)     // Before starting a track
#define REBUFFER_BYTES      (16 * 1024)     // After an underrun
#define NET_CHUNK           4096
#define NET_TIMEOUT_MS      6000
#define NET_MAX_RETRIES     5
#define DEFAULT_VOLUME      60
#define PREV_RESTART_MS     3000            // "Previous" within 3 s goes to the previous track

typedef enum { CMD_PLAY_INDEX, CMD_TOGGLE, CMD_NEXT, CMD_PREV, CMD_STOP, CMD_VOLUME } cmd_type_t;
typedef struct {
    cmd_type_t type;
    int arg;
} cmd_t;

typedef struct {
    char *url;
    uint32_t gen;
} net_req_t;

#define NET_IDLE_BIT BIT0

static SemaphoreHandle_t s_lock;            // Guards playlist, status and net result
static QueueHandle_t s_cmd_q;
static QueueHandle_t s_net_q;
static EventGroupHandle_t s_net_events;
static RingbufHandle_t s_rb;
static esp_codec_dev_handle_t s_spk;

// Playlist (guarded by s_lock)
static char *s_base_url;
static char *s_dir;
static char **s_names;
static int s_count;

static music_player_status_t s_status = { .state = MUSIC_PLAYER_STOPPED, .index = -1, .volume = DEFAULT_VOLUME };
static volatile uint32_t s_gen;             // Bumped on every track change; stale downloads stop
static uint32_t s_net_done_gen;             // Generation whose download finished (guarded by s_lock)
static bool s_net_ok;                       // ...and whether it reached the end of the file

// ---------------------------------------------------------------- download task
static void net_task(void *arg)
{
    char *buf = malloc(NET_CHUNK);
    net_req_t req;
    for (;;) {
        xEventGroupSetBits(s_net_events, NET_IDLE_BIT);
        xQueueReceive(s_net_q, &req, portMAX_DELAY);
        xEventGroupClearBits(s_net_events, NET_IDLE_BIT);

        size_t got = 0;
        int retries = 0;
        bool eof = false;
        while (req.gen == s_gen && !eof && retries <= NET_MAX_RETRIES) {
            esp_http_client_config_t cfg = { .url = req.url, .timeout_ms = NET_TIMEOUT_MS, .buffer_size = 4096 };
            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            char range[40];
            if (got > 0) {
                // Resume after a dropped connection (nginx supports byte ranges)
                snprintf(range, sizeof(range), "bytes=%u-", (unsigned)got);
                esp_http_client_set_header(client, "Range", range);
            }
            bool failed = true;
            if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
                int status = esp_http_client_get_status_code(client);
                size_t skip = (got > 0 && status == 200) ? got : 0;     // Server ignored Range
                if (status == 200 || status == 206) {
                    failed = false;
                    while (req.gen == s_gen) {
                        int r = esp_http_client_read(client, buf, NET_CHUNK);
                        if (r < 0) {
                            failed = true;
                            break;
                        }
                        if (r == 0) {
                            if (esp_http_client_is_complete_data_received(client)) {
                                eof = true;
                            } else {
                                failed = true;
                            }
                            break;
                        }
                        int off = 0;
                        if (skip > 0) {
                            off = (skip >= (size_t)r) ? r : (int)skip;
                            skip -= off;
                        }
                        // Blocks while the ring buffer is full (i.e. while paused)
                        while (off < r && req.gen == s_gen) {
                            if (xRingbufferSend(s_rb, buf + off, r - off, pdMS_TO_TICKS(100)) == pdTRUE) {
                                got += r - off;
                                off = r;
                            }
                        }
                        retries = 0;
                    }
                } else {
                    ESP_LOGE(TAG, "HTTP %d for %s", status, req.url);
                    retries = NET_MAX_RETRIES;  // Not a transient error
                }
            }
            esp_http_client_cleanup(client);
            if (failed && req.gen == s_gen) {
                retries++;
                ESP_LOGW(TAG, "Download interrupted at %u bytes, retry %d/%d", (unsigned)got, retries, NET_MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_net_done_gen = req.gen;
        s_net_ok = eof;
        xSemaphoreGive(s_lock);
        free(req.url);
    }
}

// ---------------------------------------------------------------- playback / control task
static esp_audio_simple_dec_handle_t s_dec;
static uint8_t *s_pcm;
static uint32_t s_pcm_size;
static uint32_t s_open_rate;                // Sample rate the speaker is opened with, 0 = closed
static uint64_t s_samples_played;

static void set_state(music_player_state_t st, const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = st;
    strlcpy(s_status.message, msg ? msg : "", sizeof(s_status.message));
    xSemaphoreGive(s_lock);
}

static size_t rb_used(void)
{
    return RB_SIZE - xRingbufferGetCurFreeSize(s_rb);
}

static void drain_ring_buffer(void)
{
    size_t n;
    void *p;
    while ((p = xRingbufferReceiveUpTo(s_rb, &n, 0, RB_SIZE)) != NULL) {
        vRingbufferReturnItem(s_rb, p);
    }
}

static bool decoder_reset(void)
{
    if (s_dec) {
        esp_audio_simple_dec_close(s_dec);
        s_dec = NULL;
    }
    esp_audio_simple_dec_cfg_t cfg = { .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 };
    if (esp_audio_simple_dec_open(&cfg, &s_dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Open MP3 decoder failed");
        s_dec = NULL;
        return false;
    }
    return true;
}

// Invalidate the current download and wait (while draining) until the net task is idle.
// Returns false if the decoder cannot be (re)opened.
static bool abort_track(void)
{
    s_gen++;
    for (int i = 0; i < (NET_TIMEOUT_MS + 1000) / 20; i++) {
        drain_ring_buffer();
        if (xEventGroupGetBits(s_net_events) & NET_IDLE_BIT) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    drain_ring_buffer();
    return decoder_reset();
}

static void start_track(int index)
{
    if (!abort_track()) {
        // Without a decoder every track would "finish" instantly and auto-advance forever
        set_state(MUSIC_PLAYER_ERROR, "MP3 解码器初始化失败");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_lock);
        return;
    }
    index = (index % s_count + s_count) % s_count;
    const char *name = s_names[index];
    size_t rel_len = strlen(s_dir) + strlen(name) + 1;
    char *rel = malloc(rel_len);
    snprintf(rel, rel_len, "%s%s", s_dir, name);
    char *enc = music_net_url_encode(rel);
    size_t url_len = strlen(s_base_url) + strlen(enc) + 1;
    char *url = malloc(url_len);
    snprintf(url, url_len, "%s%s", s_base_url, enc);
    free(rel);
    free(enc);

    s_status.index = index;
    strlcpy(s_status.title, name, sizeof(s_status.title));
    char *dot = strrchr(s_status.title, '.');
    if (dot) {
        *dot = '\0';
    }
    s_status.elapsed_ms = 0;
    s_status.state = MUSIC_PLAYER_BUFFERING;
    s_status.message[0] = '\0';
    s_net_done_gen = 0;
    xSemaphoreGive(s_lock);

    s_samples_played = 0;
    net_req_t req = { .url = url, .gen = s_gen };
    ESP_LOGI(TAG, "Play [%d] %s", index, url);
    xQueueSend(s_net_q, &req, portMAX_DELAY);
}

static void open_speaker(uint32_t rate)
{
    if (s_spk == NULL || rate == s_open_rate) {
        return;
    }
    if (s_open_rate) {
        esp_codec_dev_close(s_spk);
    }
    // The board's I2S path is mono: stereo MP3s are down-mixed before writing
    esp_codec_dev_sample_info_t fs = { .sample_rate = rate, .channel = 1, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_out_vol(s_spk, s_status.volume);
        s_open_rate = rate;
        ESP_LOGI(TAG, "Speaker opened at %lu Hz", (unsigned long)rate);
    } else {
        ESP_LOGE(TAG, "Open speaker at %lu Hz failed", (unsigned long)rate);
        s_open_rate = 0;
    }
}

static void output_pcm(uint32_t bytes)
{
    esp_audio_simple_dec_info_t info;
    if (esp_audio_simple_dec_get_info(s_dec, &info) != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16) {
        return;
    }
    open_speaker(info.sample_rate);
    if (s_open_rate == 0) {
        return;
    }
    int16_t *pcm = (int16_t *)s_pcm;
    uint32_t frames = bytes / 2 / info.channel;
    if (info.channel == 2) {
        for (uint32_t i = 0; i < frames; i++) {
            pcm[i] = (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2);
        }
    }
    esp_codec_dev_write(s_spk, pcm, frames * 2);    // Blocks at playback speed
    s_samples_played += frames;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.elapsed_ms = (uint32_t)(s_samples_played * 1000 / info.sample_rate);
    s_status.sample_rate = info.sample_rate;
    xSemaphoreGive(s_lock);
}

// Feed one chunk (or end-of-stream) to the decoder and play the PCM it produces
static void decode(uint8_t *data, uint32_t len, bool eos)
{
    if (s_dec == NULL) {
        return;
    }
    esp_audio_simple_dec_raw_t raw = { .buffer = data, .len = len, .eos = eos };
    do {
        esp_audio_simple_dec_out_t out = { .buffer = s_pcm, .len = s_pcm_size };
        esp_audio_err_t ret = esp_audio_simple_dec_process(s_dec, &raw, &out);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint8_t *nb = realloc(s_pcm, out.needed_size);
            if (nb == NULL) {
                return;
            }
            s_pcm = nb;
            s_pcm_size = out.needed_size;
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "Decode error %d, skipping chunk", ret);
            return;
        }
        if (out.decoded_size > 0) {
            output_pcm(out.decoded_size);
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        if (raw.consumed == 0 && out.decoded_size == 0) {
            break;  // Needs more input
        }
    } while (raw.len > 0);
}

// One step of playback; returns true when the current track has finished (or failed)
static bool play_step(bool *failed)
{
    size_t n = 0;
    uint8_t *p = xRingbufferReceiveUpTo(s_rb, &n, pdMS_TO_TICKS(20), NET_CHUNK);
    if (p != NULL) {
        decode(p, n, false);
        vRingbufferReturnItem(s_rb, p);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool done = (s_net_done_gen == s_gen);
    bool ok = s_net_ok;
    xSemaphoreGive(s_lock);
    if (done) {
        if (ok) {
            static uint8_t eos_dummy;
            decode(&eos_dummy, 0, true);    // Flush the last frames (some parsers reject NULL)
        }
        *failed = !ok;
        return true;
    }
    set_state(MUSIC_PLAYER_BUFFERING, NULL);    // Underrun: wait for more data
    return false;
}

static void control_task(void *arg)
{
    for (;;) {
        music_player_state_t st = s_status.state;
        bool active = (st == MUSIC_PLAYER_PLAYING || st == MUSIC_PLAYER_BUFFERING);
        cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, active ? 0 : pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (cmd.type) {
            case CMD_PLAY_INDEX:
                start_track(cmd.arg);
                break;
            case CMD_TOGGLE:
                if (st == MUSIC_PLAYER_PLAYING || st == MUSIC_PLAYER_BUFFERING) {
                    set_state(MUSIC_PLAYER_PAUSED, NULL);
                } else if (st == MUSIC_PLAYER_PAUSED) {
                    set_state(MUSIC_PLAYER_BUFFERING, NULL);
                } else if (s_count > 0) {
                    start_track(s_status.index >= 0 ? s_status.index : 0);
                }
                break;
            case CMD_NEXT:
                start_track(s_status.index + 1);
                break;
            case CMD_PREV:
                start_track(s_status.elapsed_ms > PREV_RESTART_MS ? s_status.index : s_status.index - 1);
                break;
            case CMD_STOP:
                abort_track();
                set_state(MUSIC_PLAYER_STOPPED, NULL);
                break;
            case CMD_VOLUME:
                s_status.volume = cmd.arg;
                if (s_open_rate) {
                    esp_codec_dev_set_out_vol(s_spk, cmd.arg);
                }
                break;
            }
            continue;   // Handle queued commands before playing more
        }

        st = s_status.state;
        if (st == MUSIC_PLAYER_BUFFERING) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool net_done = (s_net_done_gen == s_gen);
            xSemaphoreGive(s_lock);
            size_t need = (s_samples_played == 0) ? PREBUFFER_BYTES : REBUFFER_BYTES;
            if (rb_used() >= need || net_done) {
                set_state(MUSIC_PLAYER_PLAYING, NULL);
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else if (st == MUSIC_PLAYER_PLAYING) {
            bool failed = false;
            if (play_step(&failed)) {
                if (failed) {
                    ESP_LOGE(TAG, "Track %d failed", s_status.index);
                    set_state(MUSIC_PLAYER_ERROR, "网络错误，无法播放");
                } else {
                    start_track(s_status.index + 1);    // Auto-advance, wraps around
                }
            }
        }
        // PAUSED / STOPPED / ERROR: wait for commands (the receive above blocks 100 ms)
    }
}

// ---------------------------------------------------------------- public API
void music_player_init(void)
{
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(8, sizeof(cmd_t));
    s_net_q = xQueueCreate(2, sizeof(net_req_t));
    s_net_events = xEventGroupCreate();
    s_rb = xRingbufferCreateWithCaps(RB_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    s_pcm_size = 4608 * 2;
    s_pcm = malloc(s_pcm_size);
    // Order matters (see esp_audio_codec test_apps simple_decoder_test.c): the simple decoder
    // only parses frames and hands them to the registered audio decoders
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL || s_rb == NULL || s_pcm == NULL) {
        ESP_LOGE(TAG, "Init failed (speaker %p, ring buffer %p)", s_spk, s_rb);
        set_state(MUSIC_PLAYER_ERROR, "音频初始化失败");
    }
    // Stacks in internal RAM (xTaskCreate): the decoder is stack-hungry and HTTP/TLS-free here
    if (xTaskCreate(net_task, "music_net", 6 * 1024, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(control_task, "music_play", 8 * 1024, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Create player tasks failed: internal RAM free %u, largest %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        set_state(MUSIC_PLAYER_ERROR, "内存不足，无法播放");
    }
}

void music_player_play_list(const char *base_url, const char *dir, const char *const *names, int count, int start)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        free(s_names[i]);
    }
    free(s_names);
    free(s_base_url);
    free(s_dir);
    s_names = calloc(count > 0 ? count : 1, sizeof(char *));
    for (int i = 0; i < count && s_names; i++) {
        s_names[i] = strdup(names[i]);
    }
    s_count = s_names ? count : 0;
    s_base_url = strdup(base_url);
    s_dir = strdup(dir);
    s_status.count = s_count;
    xSemaphoreGive(s_lock);
    cmd_t cmd = { CMD_PLAY_INDEX, start };
    xQueueSend(s_cmd_q, &cmd, portMAX_DELAY);
}

static void send_cmd(cmd_type_t type, int arg)
{
    if (s_cmd_q == NULL) {
        return;     // music_player_init() not done yet
    }
    cmd_t cmd = { type, arg };
    xQueueSend(s_cmd_q, &cmd, pdMS_TO_TICKS(100));
}

void music_player_toggle_pause(void) { send_cmd(CMD_TOGGLE, 0); }
void music_player_next(void) { send_cmd(CMD_NEXT, 0); }
void music_player_prev(void) { send_cmd(CMD_PREV, 0); }
void music_player_stop(void) { send_cmd(CMD_STOP, 0); }

void music_player_set_volume(int volume)
{
    send_cmd(CMD_VOLUME, volume < 0 ? 0 : (volume > 100 ? 100 : volume));
}

void music_player_get_status(music_player_status_t *status)
{
    if (s_lock == NULL) {
        memset(status, 0, sizeof(*status));
        status->state = MUSIC_PLAYER_STOPPED;
        status->index = -1;
        status->volume = DEFAULT_VOLUME;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    status->buffer_pct = (int)(rb_used() * 100 / RB_SIZE);
    xSemaphoreGive(s_lock);
}
