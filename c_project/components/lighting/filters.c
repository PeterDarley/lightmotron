#include "filters.h"
#include "colors.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "filters";

static const char *filter_names[] = {
    "null", "brightness", "sizzle", "scintillate", "spike", "dropout"
};
static const int filter_count_val = 6;

void filter_null(const cJSON *params, led_output_t *target, const rgb_t *current,
                 int count, filter_state_t *state, uint32_t tick)
{
    /* No-op: leave target unchanged */
    (void)params;
    (void)current;
    (void)state;
    (void)tick;
}

void filter_brightness(const cJSON *params, led_output_t *target, const rgb_t *current,
                       int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;
    (void)tick;

    float multiplier = 1.0f;
    if (params) {
        multiplier = (float)json_get_double(params, "brightness", 1.0);
    }

    for (int i = 0; i < count; i++) {
        target[i].color.r = color_clamp((int)(target[i].color.r * multiplier));
        target[i].color.g = color_clamp((int)(target[i].color.g * multiplier));
        target[i].color.b = color_clamp((int)(target[i].color.b * multiplier));
    }
}

void filter_sizzle(const cJSON *params, led_output_t *target, const rgb_t *current,
                   int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;

    int frequency = 1;     /* Apply every N ticks */
    int variation = 20;    /* Percent variation */

    if (params) {
        frequency = json_get_int(params, "frequency", 1);
        variation = json_get_int(params, "variation_percent", 20);
    }

    if (frequency > 1 && (tick % frequency) != 0) {
        return;
    }

    /* Uniform random deviation applied to all LEDs the same */
    int deviation = (rand() % (variation * 2 + 1)) - variation;
    float factor = 1.0f + (float)deviation / 100.0f;

    for (int i = 0; i < count; i++) {
        target[i].color.r = color_clamp((int)(target[i].color.r * factor));
        target[i].color.g = color_clamp((int)(target[i].color.g * factor));
        target[i].color.b = color_clamp((int)(target[i].color.b * factor));
    }
}

void filter_scintillate(const cJSON *params, led_output_t *target, const rgb_t *current,
                        int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;

    int frequency = 1;
    int variation = 30;

    if (params) {
        frequency = json_get_int(params, "frequency", 1);
        variation = json_get_int(params, "variation_percent", 30);
    }

    if (frequency > 1 && (tick % frequency) != 0) {
        return;
    }

    /* Per-LED independent random deviation */
    for (int i = 0; i < count; i++) {
        int deviation = (rand() % (variation * 2 + 1)) - variation;
        float factor = 1.0f + (float)deviation / 100.0f;
        target[i].color.r = color_clamp((int)(target[i].color.r * factor));
        target[i].color.g = color_clamp((int)(target[i].color.g * factor));
        target[i].color.b = color_clamp((int)(target[i].color.b * factor));
    }
}

void filter_spike(const cJSON *params, led_output_t *target, const rgb_t *current,
                  int count, filter_state_t *state, uint32_t tick)
{
    (void)current;

    int frequency = 10;
    int duration = 3;
    rgb_t spike_color = {255, 255, 255};
    const char *scope = "all";

    if (params) {
        frequency = json_get_int(params, "frequency", 10);
        duration = json_get_int(params, "duration", 3);
        scope = json_get_string(params, "scope", "all");

        cJSON *color_spec = cJSON_GetObjectItem(params, "color");
        if (color_spec) {
            spike_color = color_resolve_json(color_spec);
        }
    }

    /* Check if we should trigger a new spike */
    if (!state->spike_active) {
        if ((tick - state->last_spike_tick) >= (uint32_t)frequency) {
            state->spike_active = true;
            state->last_spike_tick = tick;

            if (strcmp(scope, "individual") == 0) {
                state->spike_led_index = rand() % count;
            } else if (strcmp(scope, "subranges") == 0) {
                /* Pick a random starting subrange */
                int subrange_size = count > 4 ? count / 4 : 1;
                state->spike_subrange_index = (rand() % (count / subrange_size)) * subrange_size;
            }
        }
    }

    /* Check if spike duration has elapsed */
    if (state->spike_active && (tick - state->last_spike_tick) >= (uint32_t)duration) {
        state->spike_active = false;
    }

    /* Apply spike */
    if (state->spike_active) {
        if (strcmp(scope, "all") == 0) {
            for (int i = 0; i < count; i++) {
                target[i].color = spike_color;
            }
        } else if (strcmp(scope, "individual") == 0) {
            if (state->spike_led_index < count) {
                target[state->spike_led_index].color = spike_color;
            }
        } else if (strcmp(scope, "subranges") == 0) {
            int subrange_size = count > 4 ? count / 4 : 1;
            int start = state->spike_subrange_index;
            int end = start + subrange_size;
            if (end > count) end = count;
            for (int i = start; i < end; i++) {
                target[i].color = spike_color;
            }
        }
    }
}

void filter_dropout(const cJSON *params, led_output_t *target, const rgb_t *current,
                    int count, filter_state_t *state, uint32_t tick)
{
    /* Dropout is just a spike with black color.
     * Cache modified params in state to avoid allocation per tick. */
    if (!state->cached_params) {
        cJSON *modified = params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject();
        if (!cJSON_GetObjectItem(modified, "color")) {
            cJSON *black = cJSON_CreateArray();
            cJSON_AddItemToArray(black, cJSON_CreateNumber(0));
            cJSON_AddItemToArray(black, cJSON_CreateNumber(0));
            cJSON_AddItemToArray(black, cJSON_CreateNumber(0));
            cJSON_AddItemToObject(modified, "color", black);
        }
        state->cached_params = modified;
    }

    filter_spike((const cJSON *)state->cached_params, target, current, count, state, tick);
}

/* Filter lookup table */
typedef struct {
    const char *name;
    filter_fn_t fn;
} filter_entry_t;

static const filter_entry_t filter_table[] = {
    {"null",        filter_null},
    {"brightness",  filter_brightness},
    {"sizzle",      filter_sizzle},
    {"scintillate", filter_scintillate},
    {"spike",       filter_spike},
    {"dropout",     filter_dropout},
    {NULL, NULL},
};

filter_fn_t filter_get(const char *name)
{
    if (!name) return filter_null;

    for (int i = 0; filter_table[i].name != NULL; i++) {
        if (strcmp(name, filter_table[i].name) == 0) {
            return filter_table[i].fn;
        }
    }

    ESP_LOGW(TAG, "Unknown filter: %s, defaulting to null", name);
    return filter_null;
}

const char **filter_get_names(int *count)
{
    if (count) *count = filter_count_val;
    return filter_names;
}
