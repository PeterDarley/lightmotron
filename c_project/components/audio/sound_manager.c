#include "sound_manager.h"
#include "audio_player.h"
#include "persistent_dict.h"
#include "json_helpers.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "sound_manager";

#define MAX_PLAYING_SOUNDS 3
#define POLL_INTERVAL_MS   100  /* Matches AudioPlayer's continuous polling cadence in lib/audio.py */
#define MAX_TITLE_LEN      64

typedef struct {
    char title[MAX_TITLE_LEN];
    int module_index;
    int loops_remaining; /* Native loop_count; soundscape-driven plays force this to 0
                            since the soundscape state machine drives its own repeats. */
    int file_number;
    bool active;
} playing_sound_t;

static playing_sound_t playing_sounds[MAX_PLAYING_SOUNDS];
static SemaphoreHandle_t sound_mutex = NULL;
static TaskHandle_t poll_task_handle = NULL;
static bool initialized = false;

/* Name of the currently playing soundscape, "" if none. Mirrors
 * SoundManager._active_soundscape in lib/sounds.py. */
static char active_soundscape[MAX_TITLE_LEN] = "";

/* Per-entry soundscape progress, keyed by entry name -> object with:
 * started(bool), complete(bool), title(string), module_idx(number),
 * repeat_enabled(bool), repeat_infinite(bool), repeat_remaining(number).
 * Mirrors SoundManager._soundscape_state in lib/sounds.py. */
static cJSON *soundscape_state = NULL;

static cJSON *get_sounds_dict(void)
{
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!store) return NULL;

    cJSON *models = persistent_dict_get(store, "models");
    cJSON *current = persistent_dict_get(store, "current_model");
    if (!models || !current || !current->valuestring) return NULL;

    cJSON *model = cJSON_GetObjectItem(models, current->valuestring);
    if (!model) return NULL;

    return cJSON_GetObjectItem(model, "sounds");
}

static cJSON *get_sound_by_title(const char *title)
{
    cJSON *sounds = get_sounds_dict();
    if (!sounds) return NULL;
    return cJSON_GetObjectItem(sounds, title);
}

static cJSON *get_soundscapes_dict(void)
{
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!store) return NULL;

    cJSON *models = persistent_dict_get(store, "models");
    cJSON *current = persistent_dict_get(store, "current_model");
    if (!models || !current || !current->valuestring) return NULL;

    cJSON *model = cJSON_GetObjectItem(models, current->valuestring);
    if (!model) return NULL;

    return cJSON_GetObjectItem(model, "soundscapes");
}

static cJSON *get_soundscape_by_name(const char *name)
{
    cJSON *soundscapes = get_soundscapes_dict();
    if (!soundscapes) return NULL;
    return cJSON_GetObjectItem(soundscapes, name);
}

esp_err_t sound_manager_init(void)
{
    if (initialized) return ESP_OK;

    sound_mutex = xSemaphoreCreateMutex();
    memset(playing_sounds, 0, sizeof(playing_sounds));
    active_soundscape[0] = '\0';
    initialized = true;

    ESP_LOGI(TAG, "Sound manager initialized");
    return ESP_OK;
}

/* Resolve and start playback of `sound` (already looked up by title), stopping
 * any sounds in its "stops" list first, then track it in playing_sounds[].
 * `loop_count` is the effective native loop count to arm; soundscape entries
 * pass 0 since the soundscape state machine (not the native loop mechanism)
 * drives their repeats. Mirrors the shared body of SoundManager.play_sound. */
static esp_err_t start_sound_playback(const char *title, cJSON *sound, int loop_count, int *out_module_idx)
{
    int file_number = json_get_int(sound, "file", 0);
    bool high_quality = json_get_bool(sound, "high_quality", false);

    cJSON *stops = cJSON_GetObjectItem(sound, "stops");
    if (stops && cJSON_IsArray(stops)) {
        for (cJSON *item = stops->child; item; item = item->next) {
            if (item->valuestring) sound_manager_stop(item->valuestring);
        }
    }

    int module_idx = audio_player_play_file(file_number, high_quality);
    if (module_idx < 0) {
        ESP_LOGW(TAG, "No module available for sound '%s'", title);
        return ESP_FAIL;
    }

    xSemaphoreTake(sound_mutex, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (!playing_sounds[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* All slots full - stop the oldest and reuse its slot. */
        slot = 0;
        audio_player_stop_module(playing_sounds[slot].module_index);
        playing_sounds[slot].active = false;
    }

    strncpy(playing_sounds[slot].title, title, sizeof(playing_sounds[slot].title) - 1);
    playing_sounds[slot].title[sizeof(playing_sounds[slot].title) - 1] = '\0';
    playing_sounds[slot].module_index = module_idx;
    playing_sounds[slot].loops_remaining = loop_count;
    playing_sounds[slot].file_number = file_number;
    playing_sounds[slot].active = true;

    xSemaphoreGive(sound_mutex);

    ESP_LOGI(TAG, "Playing sound '%s' file=%d module=%d loops=%d",
             title, file_number, module_idx, loop_count);
    if (out_module_idx) *out_module_idx = module_idx;
    return ESP_OK;
}

esp_err_t sound_manager_play(const char *title, int loop_count_override)
{
    if (!title || !initialized) return ESP_ERR_INVALID_STATE;

    cJSON *sound = get_sound_by_title(title);
    if (!sound) {
        ESP_LOGW(TAG, "Sound not found: '%s'", title);
        return ESP_ERR_NOT_FOUND;
    }

    int loop_count = (loop_count_override > 0) ? loop_count_override
                     : json_get_int(sound, "loop_count", 0);

    return start_sound_playback(title, sound, loop_count, NULL);
}

esp_err_t sound_manager_stop(const char *title)
{
    if (!title || !initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active && strcmp(playing_sounds[i].title, title) == 0) {
            audio_player_stop_module(playing_sounds[i].module_index);
            playing_sounds[i].active = false;
            ESP_LOGI(TAG, "Stopped sound '%s'", title);

            /* If a soundscape is active, stopping any tracked sound also ends
             * it, mirroring SoundManager.stop_sound in lib/sounds.py. */
            if (active_soundscape[0] != '\0') {
                active_soundscape[0] = '\0';
                if (soundscape_state) {
                    cJSON_Delete(soundscape_state);
                    soundscape_state = NULL;
                }
            }
            break;
        }
    }

    xSemaphoreGive(sound_mutex);
    return ESP_OK;
}

void sound_manager_stop_all(void)
{
    if (!initialized) return;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    audio_player_stop_all();
    memset(playing_sounds, 0, sizeof(playing_sounds));
    active_soundscape[0] = '\0';
    if (soundscape_state) {
        cJSON_Delete(soundscape_state);
        soundscape_state = NULL;
    }
    xSemaphoreGive(sound_mutex);
}

bool sound_manager_is_playing(const char *title)
{
    if (!title || !initialized) return false;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active && strcmp(playing_sounds[i].title, title) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(sound_mutex);
    return found;
}

cJSON *sound_manager_get_playing(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!initialized) return arr;

    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(playing_sounds[i].title));
        }
    }
    xSemaphoreGive(sound_mutex);
    return arr;
}

/* Replace (or create) the state object tracked for a soundscape entry. */
static void set_entry_state(const char *entry_name, bool started, bool complete,
                             const char *title, int module_idx,
                             bool repeat_enabled, bool repeat_infinite, int repeat_remaining)
{
    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    if (!soundscape_state) soundscape_state = cJSON_CreateObject();

    cJSON *st = cJSON_CreateObject();
    cJSON_AddBoolToObject(st, "started", started);
    cJSON_AddBoolToObject(st, "complete", complete);
    cJSON_AddStringToObject(st, "title", title ? title : "");
    cJSON_AddNumberToObject(st, "module_idx", module_idx);
    cJSON_AddBoolToObject(st, "repeat_enabled", repeat_enabled);
    cJSON_AddBoolToObject(st, "repeat_infinite", repeat_infinite);
    cJSON_AddNumberToObject(st, "repeat_remaining", repeat_remaining);

    cJSON_DeleteItemFromObject(soundscape_state, entry_name);
    cJSON_AddItemToObject(soundscape_state, entry_name, st);
    xSemaphoreGive(sound_mutex);
}

static void mark_entry_complete(const char *entry_name)
{
    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    if (soundscape_state) {
        cJSON *st = cJSON_GetObjectItem(soundscape_state, entry_name);
        if (st) cJSON_ReplaceItemInObject(st, "complete", cJSON_CreateBool(true));
    }
    xSemaphoreGive(sound_mutex);
}

/* Play the next runnable entry of a soundscape (first not-yet-started entry
 * whose "after" dependency, if any, has completed; or a started entry that
 * still needs to repeat). Returns true if an entry was (re)played, false if
 * the soundscape has no more work and is now complete. Mirrors
 * SoundManager._play_next_soundscape_entry in lib/sounds.py. */
static bool play_next_soundscape_entry(const char *soundscape_name)
{
    cJSON *soundscape = get_soundscape_by_name(soundscape_name);
    if (!soundscape) {
        xSemaphoreTake(sound_mutex, portMAX_DELAY);
        active_soundscape[0] = '\0';
        xSemaphoreGive(sound_mutex);
        return false;
    }

    for (cJSON *entry = soundscape->child; entry; entry = entry->next) {
        if (!cJSON_IsObject(entry)) continue;
        const char *entry_name = entry->string;
        if (!entry_name) continue;

        xSemaphoreTake(sound_mutex, portMAX_DELAY);
        cJSON *entry_state = soundscape_state ? cJSON_GetObjectItem(soundscape_state, entry_name) : NULL;
        bool is_started = entry_state && cJSON_IsTrue(cJSON_GetObjectItem(entry_state, "started"));
        bool is_complete = entry_state && cJSON_IsTrue(cJSON_GetObjectItem(entry_state, "complete"));
        xSemaphoreGive(sound_mutex);

        if (!is_started) {
            const char *after_entry = json_get_string(entry, "after", NULL);
            if (after_entry && after_entry[0] != '\0') {
                xSemaphoreTake(sound_mutex, portMAX_DELAY);
                cJSON *after_state = soundscape_state ? cJSON_GetObjectItem(soundscape_state, after_entry) : NULL;
                bool after_complete = after_state && cJSON_IsTrue(cJSON_GetObjectItem(after_state, "complete"));
                xSemaphoreGive(sound_mutex);
                if (!after_complete) continue; /* predecessor hasn't finished yet */
            }

            const char *sound_title = json_get_string(entry, "sound", "");
            int repeat_count = json_get_int(entry, "repeat", 0);
            bool repeat_enabled = cJSON_GetObjectItem(entry, "repeat_enabled")
                                       ? json_get_bool(entry, "repeat_enabled", false)
                                       : (repeat_count > 0); /* back-compat for older entries */
            bool repeat_infinite = repeat_enabled && repeat_count == 0;

            if (sound_title[0] == '\0') {
                set_entry_state(entry_name, true, true, "", -1, false, false, 0);
                continue;
            }

            cJSON *sound = get_sound_by_title(sound_title);
            int module_idx = -1;
            if (!sound || start_sound_playback(sound_title, sound, 0, &module_idx) != ESP_OK) {
                ESP_LOGW(TAG, "soundscape: failed to play entry='%s' sound='%s'", entry_name, sound_title);
                set_entry_state(entry_name, true, true, "", -1, false, false, 0);
                continue;
            }

            set_entry_state(entry_name, true, false, sound_title, module_idx,
                             repeat_enabled, repeat_infinite, repeat_count);
            ESP_LOGI(TAG, "soundscape: playing entry='%s' sound='%s' repeat_enabled=%d infinite=%d count=%d",
                     entry_name, sound_title, repeat_enabled, repeat_infinite, repeat_count);
            return true;
        }

        if (is_complete) continue;

        /* Entry already started - check if it needs to repeat. */
        xSemaphoreTake(sound_mutex, portMAX_DELAY);
        cJSON *st = cJSON_GetObjectItem(soundscape_state, entry_name);
        bool repeat_enabled = st && cJSON_IsTrue(cJSON_GetObjectItem(st, "repeat_enabled"));
        bool repeat_infinite = st && cJSON_IsTrue(cJSON_GetObjectItem(st, "repeat_infinite"));
        int repeat_remaining = st ? json_get_int(st, "repeat_remaining", 0) : 0;
        char sound_title[MAX_TITLE_LEN] = "";
        if (st) strncpy(sound_title, json_get_string(st, "title", ""), sizeof(sound_title) - 1);
        xSemaphoreGive(sound_mutex);

        if (!repeat_enabled) {
            mark_entry_complete(entry_name);
            continue;
        }

        if (repeat_infinite || repeat_remaining > 0) {
            cJSON *sound = get_sound_by_title(sound_title);
            int module_idx = -1;
            if (!sound || start_sound_playback(sound_title, sound, 0, &module_idx) != ESP_OK) {
                ESP_LOGW(TAG, "soundscape: failed to repeat entry='%s'", entry_name);
                mark_entry_complete(entry_name);
                continue;
            }

            xSemaphoreTake(sound_mutex, portMAX_DELAY);
            cJSON *st2 = cJSON_GetObjectItem(soundscape_state, entry_name);
            if (st2) {
                cJSON_ReplaceItemInObject(st2, "module_idx", cJSON_CreateNumber(module_idx));
                if (!repeat_infinite) {
                    int remaining = repeat_remaining - 1;
                    cJSON_ReplaceItemInObject(st2, "repeat_remaining", cJSON_CreateNumber(remaining));
                    if (remaining <= 0) {
                        cJSON_ReplaceItemInObject(st2, "complete", cJSON_CreateBool(true));
                    }
                }
            }
            xSemaphoreGive(sound_mutex);

            ESP_LOGI(TAG, "soundscape: repeating entry='%s' sound='%s' infinite=%d remaining=%d",
                     entry_name, sound_title, repeat_infinite, repeat_remaining - 1);
            return true;
        }

        mark_entry_complete(entry_name);
    }

    /* No more entries to play. */
    ESP_LOGI(TAG, "soundscape: complete name='%s'", soundscape_name);
    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    active_soundscape[0] = '\0';
    if (soundscape_state) {
        cJSON_Delete(soundscape_state);
        soundscape_state = NULL;
    }
    xSemaphoreGive(sound_mutex);
    return false;
}

esp_err_t sound_manager_play_soundscape(const char *name)
{
    if (!name || !initialized) return ESP_ERR_INVALID_STATE;

    cJSON *soundscape = get_soundscape_by_name(name);
    if (!soundscape) {
        ESP_LOGW(TAG, "soundscape: not found name='%s'", name);
        return ESP_ERR_NOT_FOUND;
    }

    sound_manager_stop_all();

    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    strncpy(active_soundscape, name, sizeof(active_soundscape) - 1);
    active_soundscape[sizeof(active_soundscape) - 1] = '\0';
    if (soundscape_state) cJSON_Delete(soundscape_state);
    soundscape_state = cJSON_CreateObject();
    xSemaphoreGive(sound_mutex);

    ESP_LOGI(TAG, "soundscape: start name='%s'", name);
    play_next_soundscape_entry(name);
    return ESP_OK;
}

const char *sound_manager_get_active_soundscape(void)
{
    if (active_soundscape[0] == '\0') return "";

    /* Self-heal stale state: if nothing is playing on any module, the
     * soundscape should have advanced already - do it now. Mirrors
     * SoundManager.get_active_soundscape in lib/sounds.py. */
    bool any_playing = false;
    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active) {
            any_playing = true;
            break;
        }
    }
    xSemaphoreGive(sound_mutex);

    if (!any_playing) {
        char name_copy[MAX_TITLE_LEN];
        strncpy(name_copy, active_soundscape, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';
        play_next_soundscape_entry(name_copy);
    }

    return active_soundscape;
}

/* Handle end-of-playback for a sound: native loop, soundscape advancement, or
 * chain_next, in that priority order. Mirrors SoundManager.handle_sound_ended
 * in lib/sounds.py. */
static void handle_sound_ended(const char *title, int module_idx, int file_number, int loops_remaining)
{
    if (loops_remaining > 0) {
        int new_module = audio_player_play_file(file_number, false);

        xSemaphoreTake(sound_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
            if (playing_sounds[i].active && playing_sounds[i].module_index == module_idx &&
                strcmp(playing_sounds[i].title, title) == 0) {
                if (new_module >= 0) {
                    playing_sounds[i].module_index = new_module;
                    playing_sounds[i].loops_remaining = loops_remaining - 1;
                } else {
                    playing_sounds[i].active = false;
                }
                break;
            }
        }
        xSemaphoreGive(sound_mutex);
        ESP_LOGD(TAG, "sound: loop title='%s' remaining=%d", title, loops_remaining - 1);
        return;
    }

    /* No more native loops - free this slot before considering soundscape/chain. */
    xSemaphoreTake(sound_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
        if (playing_sounds[i].active && playing_sounds[i].module_index == module_idx &&
            strcmp(playing_sounds[i].title, title) == 0) {
            playing_sounds[i].active = false;
            break;
        }
    }
    xSemaphoreGive(sound_mutex);

    /* Soundscape advancement takes priority over chaining, but only when the
     * sound that ended matches an active (not-yet-complete) soundscape entry. */
    if (active_soundscape[0] != '\0') {
        bool matched = false;

        xSemaphoreTake(sound_mutex, portMAX_DELAY);
        if (soundscape_state) {
            for (cJSON *entry_state = soundscape_state->child; entry_state; entry_state = entry_state->next) {
                if (cJSON_IsTrue(cJSON_GetObjectItem(entry_state, "complete"))) continue;
                const char *expected_title = json_get_string(entry_state, "title", "");
                int expected_module = json_get_int(entry_state, "module_idx", -1);
                if (strcmp(expected_title, title) == 0 &&
                    (expected_module < 0 || expected_module == module_idx)) {
                    matched = true;
                    break;
                }
            }
        }
        char name_copy[MAX_TITLE_LEN];
        strncpy(name_copy, active_soundscape, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';
        xSemaphoreGive(sound_mutex);

        if (matched) {
            play_next_soundscape_entry(name_copy);
            return;
        }
    }

    /* Chain to the configured next sound, if any. */
    cJSON *sound = get_sound_by_title(title);
    if (sound) {
        const char *chain_next = json_get_string(sound, "chain_next", NULL);
        if (chain_next && chain_next[0] != '\0') {
            ESP_LOGI(TAG, "sound: chain from '%s' to '%s'", title, chain_next);
            char chain_next_copy[MAX_TITLE_LEN];
            strncpy(chain_next_copy, chain_next, sizeof(chain_next_copy) - 1);
            chain_next_copy[sizeof(chain_next_copy) - 1] = '\0';
            if (sound_manager_play(chain_next_copy, 0) != ESP_OK) {
                ESP_LOGW(TAG, "sound: chain failed from '%s' to '%s'", title, chain_next_copy);
            }
            return;
        }
    }

    ESP_LOGD(TAG, "sound: ended title='%s' module=%d", title, module_idx);
}

static void poll_task(void *arg)
{
    typedef struct {
        char title[MAX_TITLE_LEN];
        int module_index;
        int file_number;
        int loops_remaining;
    } ended_sound_t;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        ended_sound_t ended[MAX_PLAYING_SOUNDS];
        int ended_count = 0;

        xSemaphoreTake(sound_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_PLAYING_SOUNDS; i++) {
            if (!playing_sounds[i].active) continue;
            if (!audio_player_is_module_playing(playing_sounds[i].module_index)) {
                strncpy(ended[ended_count].title, playing_sounds[i].title, MAX_TITLE_LEN - 1);
                ended[ended_count].title[MAX_TITLE_LEN - 1] = '\0';
                ended[ended_count].module_index = playing_sounds[i].module_index;
                ended[ended_count].file_number = playing_sounds[i].file_number;
                ended[ended_count].loops_remaining = playing_sounds[i].loops_remaining;
                ended_count++;
            }
        }
        xSemaphoreGive(sound_mutex);

        /* Handle side effects (loop/soundscape/chain) outside the lock so they
         * can freely call back into sound_manager_* helpers. */
        for (int i = 0; i < ended_count; i++) {
            handle_sound_ended(ended[i].title, ended[i].module_index,
                                ended[i].file_number, ended[i].loops_remaining);
        }
    }
}

esp_err_t sound_manager_start_polling(void)
{
    if (poll_task_handle) return ESP_OK;

    xTaskCreate(poll_task, "snd_poll", 3072, NULL, 3, &poll_task_handle);
    return ESP_OK;
}
