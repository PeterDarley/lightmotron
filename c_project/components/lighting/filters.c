#include "filters.h"
#include "colors.h"
#include "json_helpers.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "filters";

static const char *filter_names[] = {
    "null", "brightness", "sizzle", "scintillate", "spike", "dropout",
    "afterglow", "tint", "shimmer", "hue_shift", "saturation", "vignette"
};
static const int filter_count_val = 12;

/**
 * Return variation percent (0-100) from filter params, mirroring
 * FilterMixin._variation_percent(). Prefers the modern "variation_percent"
 * float field; falls back to the legacy 0-255 "variation" field (converted
 * to a percentage) for backward compatibility; otherwise returns
 * default_percent.
 */
static float filter_variation_percent(const cJSON *params, float default_percent)
{
    if (!params) return default_percent;

    if (cJSON_HasObjectItem(params, "variation_percent")) {
        double percent = json_get_double(params, "variation_percent", default_percent);
        if (percent < 0.0) percent = 0.0;
        if (percent > 100.0) percent = 100.0;
        return (float)percent;
    }

    if (cJSON_HasObjectItem(params, "variation")) {
        double legacy_levels = json_get_double(params, "variation", 0.0);
        double percent = (legacy_levels / 255.0) * 100.0;
        if (percent < 0.0) percent = 0.0;
        if (percent > 100.0) percent = 100.0;
        return (float)percent;
    }

    return default_percent;
}

/** Uniform random float in [-range, range], mirroring random.uniform(-r, r). */
static float uniform_signed(float range)
{
    if (range <= 0.0f) return 0.0f;
    float unit = (float)rand() / (float)RAND_MAX; /* [0, 1] */
    return (unit * 2.0f - 1.0f) * range;
}

void filter_null(const cJSON *params, led_output_t *target, const rgb_t *current,
                 int count, filter_state_t *state, uint32_t tick)
{
    /* No-op: leave target unchanged */
    (void)params;
    (void)target;
    (void)current;
    (void)count;
    (void)state;
    (void)tick;
}

void filter_brightness(const cJSON *params, led_output_t *target, const rgb_t *current,
                       int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;
    (void)tick;

    if (count <= 0) return;

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
    /* Uniform random deviation applied identically to every LED: one random
     * factor per channel per update, matching FilterMixin.filter_sizzle().
     * `tick` is the effect's local tick (see lighting.c's filter_fn call),
     * matching Python's tick_number=local_tick. */
    (void)current;
    (void)state;

    if (count <= 0) return;

    int frequency = params ? json_get_int(params, "frequency", 40) : 40;
    float variation_percent = filter_variation_percent(params, 20.0f);
    float variation_ratio = variation_percent / 100.0f;

    int interval = frequency > 0 ? (40 / frequency) : 1;
    if (interval < 1) interval = 1;

    if ((tick % (uint32_t)interval) != 0) return;

    float dev_r = uniform_signed(variation_ratio);
    float dev_g = uniform_signed(variation_ratio);
    float dev_b = uniform_signed(variation_ratio);

    for (int i = 0; i < count; i++) {
        target[i].color.r = color_clamp((int)roundf(target[i].color.r * (1.0f + dev_r)));
        target[i].color.g = color_clamp((int)roundf(target[i].color.g * (1.0f + dev_g)));
        target[i].color.b = color_clamp((int)roundf(target[i].color.b * (1.0f + dev_b)));
    }
}

void filter_scintillate(const cJSON *params, led_output_t *target, const rgb_t *current,
                        int count, filter_state_t *state, uint32_t tick)
{
    /* Independent random deviation per LED per channel, matching
     * FilterMixin.filter_scintillate(). */
    (void)current;
    (void)state;

    if (count <= 0) return;

    int frequency = params ? json_get_int(params, "frequency", 40) : 40;
    float variation_percent = filter_variation_percent(params, 20.0f);
    float variation_ratio = variation_percent / 100.0f;

    int interval = frequency > 0 ? (40 / frequency) : 1;
    if (interval < 1) interval = 1;

    if ((tick % (uint32_t)interval) != 0) return;

    for (int i = 0; i < count; i++) {
        float dev_r = uniform_signed(variation_ratio);
        float dev_g = uniform_signed(variation_ratio);
        float dev_b = uniform_signed(variation_ratio);

        target[i].color.r = color_clamp((int)roundf(target[i].color.r * (1.0f + dev_r)));
        target[i].color.g = color_clamp((int)roundf(target[i].color.g * (1.0f + dev_g)));
        target[i].color.b = color_clamp((int)roundf(target[i].color.b * (1.0f + dev_b)));
    }
}

/**
 * Shared spike/dropout implementation, mirroring
 * FilterMixin._apply_spike_filter(). Every independently-timed group (scope
 * "all" = 1 group, "subranges" = contiguous index runs within `target`,
 * anything else = one group per LED) gets its own next_spike/spike_end
 * state in filter_state_t.spike_groups.
 *
 * Known simplification vs Python: Python can look up explicit
 * "_target_groups" for an aggregate "named:" target composed of several
 * named subranges, so each named subrange gets its own group even when
 * physically contiguous. This job model doesn't retain that aggregate
 * grouping, so "subranges" always falls back to contiguous-index detection
 * (which is also Python's own fallback when no explicit groups are given).
 *
 * Groups beyond MAX_SPIKE_GROUPS share the last state slot (and, for
 * position-contiguous groups, are folded into that slot's LED range) -
 * documented in lighting.h.
 */
static void apply_spike_filter(const cJSON *params, led_output_t *target, int count,
                                filter_state_t *state, uint32_t tick, rgb_t spike_color)
{
    if (count <= 0) return;

    int duration = params ? json_get_int(params, "duration", 5) : 5;
    int period = params ? json_get_int(params, "period", 40) : 40;
    int variation = params ? json_get_int(params, "variation", 0) : 0;
    int heat = params ? json_get_int(params, "heat", 0) : 0;
    const char *scope = params ? json_get_string(params, "scope", "all") : "all";

    bool scope_all = strcmp(scope, "all") == 0;
    bool scope_subranges = strcmp(scope, "subranges") == 0;

    int group_start[MAX_SPIKE_GROUPS];
    int group_len[MAX_SPIKE_GROUPS];
    int group_count = 0;

    if (scope_all) {
        group_start[0] = 0;
        group_len[0] = count;
        group_count = 1;
    } else if (scope_subranges) {
        int i = 0;
        while (i < count) {
            int start = i;
            int j = i + 1;
            while (j < count && target[j].led_index == target[j - 1].led_index + 1) j++;

            if (group_count < MAX_SPIKE_GROUPS) {
                group_start[group_count] = start;
                group_len[group_count] = j - start;
                group_count++;
            } else {
                group_len[MAX_SPIKE_GROUPS - 1] += (j - start);
            }
            i = j;
        }
    } else {
        /* One LED per group ("individual" or any other scope value). */
        for (int i = 0; i < count; i++) {
            if (i < MAX_SPIKE_GROUPS) {
                group_start[i] = i;
                group_len[i] = 1;
                group_count++;
            } else {
                group_len[MAX_SPIKE_GROUPS - 1] += 1;
            }
        }
    }

    for (int g = 0; g < group_count; g++) {
        spike_group_state_t *gs = &state->spike_groups[g];

        if (!gs->initialized) {
            /* First trigger counted from effect start, without variation
             * jitter, but with a random phase offset for non-"all" scopes
             * so groups don't all spike in lockstep initially. */
            int initial_phase_offset = 0;
            if (!scope_all && period > 1) {
                initial_phase_offset = rand() % period;
            }
            gs->next_spike = (int)tick + period + initial_phase_offset;
            gs->spike_end = -1;
            gs->initialized = true;
        }

        if ((int)tick >= gs->next_spike && (int)tick > gs->spike_end) {
            int heat_offset = heat > 0 ? (rand() % (2 * heat + 1) - heat) : 0;
            int spike_duration = duration + heat_offset;
            if (spike_duration < 1) spike_duration = 1;
            gs->spike_end = (int)tick + spike_duration - 1;

            int variation_offset = variation > 0 ? (rand() % (2 * variation + 1) - variation) : 0;
            gs->next_spike = gs->spike_end + 1 + period + variation_offset;
        }

        bool active = (int)tick <= gs->spike_end;
        if (!active) continue;

        int start = group_start[g];
        int len = group_len[g];
        for (int i = start; i < start + len && i < count; i++) {
            target[i].color = spike_color;
        }
    }
}

void filter_spike(const cJSON *params, led_output_t *target, const rgb_t *current,
                  int count, filter_state_t *state, uint32_t tick)
{
    (void)current;

    rgb_t spike_color = (rgb_t){255, 255, 255}; /* default_color = "white" */
    cJSON *color_spec = params ? cJSON_GetObjectItem(params, "color") : NULL;
    if (color_spec) {
        /* color_resolve_json() already handles a plain name string, a
         * "custom:name" string, or a 3-element [r, g, b] array. */
        spike_color = color_resolve_json(color_spec);
    }

    apply_spike_filter(params, target, count, state, tick, spike_color);
}

void filter_dropout(const cJSON *params, led_output_t *target, const rgb_t *current,
                    int count, filter_state_t *state, uint32_t tick)
{
    /* Dropout is spike with the color forced to black, regardless of any
     * "color" field present in params (mirrors FilterMixin.filter_dropout()
     * passing (0, 0, 0) directly rather than resolving filter_dict["color"]).
     * The modified params are cached once per filter instance to avoid a
     * per-tick allocation. */
    if (!state->cached_params) {
        cJSON *modified = params ? cJSON_Duplicate(params, 1) : cJSON_CreateObject();

        cJSON *existing_color = cJSON_GetObjectItem(modified, "color");
        if (existing_color) {
            cJSON_DeleteItemFromObject(modified, "color");
        }

        cJSON *black = cJSON_CreateArray();
        cJSON_AddItemToArray(black, cJSON_CreateNumber(0));
        cJSON_AddItemToArray(black, cJSON_CreateNumber(0));
        cJSON_AddItemToArray(black, cJSON_CreateNumber(0));
        cJSON_AddItemToObject(modified, "color", black);

        state->cached_params = modified;
    }

    apply_spike_filter((const cJSON *)state->cached_params, target, count, state, tick,
                        (rgb_t){0, 0, 0});
}

/* ---------------------------------------------------------------------
 * New filters (mirror the filter_*() additions in lib/lighting/filters.py).
 * `current[i]` is the previous frame's logical color for target[i]'s LED
 * (parallel array), matching Python's get_logical_color(led_index).
 * ------------------------------------------------------------------- */

void filter_afterglow(const cJSON *params, led_output_t *target, const rgb_t *current,
                      int count, filter_state_t *state, uint32_t tick)
{
    (void)state;
    (void)tick;
    if (count <= 0) return;

    float decay = params ? (float)json_get_double(params, "decay", 0.85) : 0.85f;
    if (decay < 0.0f) decay = 0.0f;
    if (decay > 0.99f) decay = 0.99f;

    for (int i = 0; i < count; i++) {
        int fr = (int)(current[i].r * decay);
        int fg = (int)(current[i].g * decay);
        int fb = (int)(current[i].b * decay);
        if (target[i].color.r < fr) target[i].color.r = (uint8_t)fr;
        if (target[i].color.g < fg) target[i].color.g = (uint8_t)fg;
        if (target[i].color.b < fb) target[i].color.b = (uint8_t)fb;
    }
}

void filter_tint(const cJSON *params, led_output_t *target, const rgb_t *current,
                 int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;
    (void)tick;
    if (count <= 0) return;

    cJSON *color_item = params ? cJSON_GetObjectItem(params, "color") : NULL;
    rgb_t tint = color_item ? color_resolve_json(color_item) : color_resolve("red");
    float strength = params ? (float)json_get_double(params, "strength", 0.5) : 0.5f;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;

    for (int i = 0; i < count; i++) {
        int gr = target[i].color.r * tint.r / 255;
        int gg = target[i].color.g * tint.g / 255;
        int gb = target[i].color.b * tint.b / 255;
        target[i].color.r = color_clamp((int)(target[i].color.r * (1.0f - strength) + gr * strength));
        target[i].color.g = color_clamp((int)(target[i].color.g * (1.0f - strength) + gg * strength));
        target[i].color.b = color_clamp((int)(target[i].color.b * (1.0f - strength) + gb * strength));
    }
}

void filter_shimmer(const cJSON *params, led_output_t *target, const rgb_t *current,
                    int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;
    if (count <= 0) return;

    float wavelength = params ? (float)json_get_double(params, "wavelength", 8.0) : 8.0f;
    if (wavelength < 1.0f) wavelength = 1.0f;
    float speed = params ? (float)json_get_double(params, "speed", 1.0) : 1.0f;
    float depth = params ? (float)json_get_double(params, "depth", 0.5) : 0.5f;
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;

    for (int i = 0; i < count; i++) {
        float phase = ((float)target[i].led_index / wavelength) - ((float)tick * speed / 20.0f);
        float wave = (sinf(2.0f * (float)M_PI * phase) + 1.0f) / 2.0f;
        float factor = 1.0f - depth * (1.0f - wave);
        target[i].color.r = color_clamp((int)(target[i].color.r * factor));
        target[i].color.g = color_clamp((int)(target[i].color.g * factor));
        target[i].color.b = color_clamp((int)(target[i].color.b * factor));
    }
}

void filter_hue_shift(const cJSON *params, led_output_t *target, const rgb_t *current,
                      int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;
    if (count <= 0) return;

    float speed = params ? (float)json_get_double(params, "speed", 0.0) : 0.0f;
    float offset = params ? (float)json_get_double(params, "offset", 0.0) : 0.0f;
    float shift = offset + ((float)tick * speed / 40.0f);
    shift -= floorf(shift);

    for (int i = 0; i < count; i++) {
        float h, s, v;
        color_to_hsv(target[i].color, &h, &s, &v);
        target[i].color = color_from_hsv(h + shift, s, v);
    }
}

void filter_saturation(const cJSON *params, led_output_t *target, const rgb_t *current,
                       int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;
    (void)tick;
    if (count <= 0) return;

    float amount = params ? (float)json_get_double(params, "amount", 1.0) : 1.0f;
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 2.0f) amount = 2.0f;

    for (int i = 0; i < count; i++) {
        float h, s, v;
        color_to_hsv(target[i].color, &h, &s, &v);
        float ns = s * amount;
        if (ns < 0.0f) ns = 0.0f;
        if (ns > 1.0f) ns = 1.0f;
        target[i].color = color_from_hsv(h, ns, v);
    }
}

void filter_vignette(const cJSON *params, led_output_t *target, const rgb_t *current,
                     int count, filter_state_t *state, uint32_t tick)
{
    (void)current;
    (void)state;
    (void)tick;
    if (count <= 0) return;

    float falloff = params ? (float)json_get_double(params, "falloff", 0.5) : 0.5f;
    if (falloff < 0.0f) falloff = 0.0f;
    if (falloff > 1.0f) falloff = 1.0f;
    bool invert = params ? json_get_bool(params, "invert", false) : false;

    for (int i = 0; i < count; i++) {
        float position = (float)i / (float)(count > 1 ? count - 1 : 1);
        float edge = fabsf(position - 0.5f) * 2.0f;
        if (invert) edge = 1.0f - edge;
        float factor = 1.0f - falloff * edge;
        target[i].color.r = color_clamp((int)(target[i].color.r * factor));
        target[i].color.g = color_clamp((int)(target[i].color.g * factor));
        target[i].color.b = color_clamp((int)(target[i].color.b * factor));
    }
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
    {"afterglow",   filter_afterglow},
    {"tint",        filter_tint},
    {"shimmer",     filter_shimmer},
    {"hue_shift",   filter_hue_shift},
    {"saturation",  filter_saturation},
    {"vignette",    filter_vignette},
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
