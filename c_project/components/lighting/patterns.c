#include "patterns.h"
#include "colors.h"
#include "esp_log.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "patterns";

static const char *pattern_names[] = {
    "solid", "blink", "pulse", "fade_in", "breathe", "wave", "cylon", "phaser_strip"
};
static const int pattern_count = 8;

/* Max number of simultaneous wave "heads" pattern_wave will render (mirrors
 * the effect's "number" param). Extra heads beyond this are silently
 * dropped rather than overflowing a stack array. */
#define MAX_WAVE_HEADS 8

/* Scratch buffers for phaser_strip's converging left/right wave renders.
 * Static (not stack-allocated) to avoid a large per-tick stack footprint,
 * matching the "pre-allocated buffers" convention used by lighting.c's tick
 * processing. Safe because lighting_process_tick holds a mutex for the
 * whole tick and patterns are never called re-entrantly. */
static led_output_t phaser_left_buf[MAX_LEDS];
static led_output_t phaser_right_buf[MAX_LEDS];

int pattern_solid(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                  led_output_t *output, int max_output)
{
    (void)local_tick;
    (void)current_colors;

    rgb_t color = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    int count = 0;

    for (int i = 0; i < job->target_count && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color;
        count++;
    }

    return count;
}

int pattern_blink(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                  led_output_t *output, int max_output)
{
    (void)current_colors;

    /* job->duration holds the blink "half period" (on-ticks == off-ticks),
     * matching Python's pattern_blink() which reads effect["duration"]
     * directly as half_period (with a default of 40, not a generic
     * pattern-duration default). Legacy "frequency" overrides are folded
     * into job->duration once at effect_resolve() time. */
    int duration = job->duration > 0 ? job->duration : 40;
    bool on = ((local_tick / duration) % 2) == 0;

    rgb_t color;
    if (on) {
        color = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    } else {
        color = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};
    }

    int count = 0;
    for (int i = 0; i < job->target_count && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color;
        count++;
    }

    return count;
}

int pattern_pulse(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                  led_output_t *output, int max_output)
{
    (void)current_colors;

    /* Matches Python pattern_pulse(): on_ticks defaults to 10, period
     * defaults flatly to 40 (not derived from on_ticks). */
    int duration = job->duration > 0 ? job->duration : 10;
    int period = job->period > 0 ? job->period : 40;
    int phase = local_tick % period;
    bool on = phase < duration;

    rgb_t color;
    if (on) {
        color = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    } else {
        color = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};
    }

    int count = 0;
    for (int i = 0; i < job->target_count && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color;
        count++;
    }

    return count;
}

int pattern_fade_in(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                    led_output_t *output, int max_output)
{
    (void)current_colors;

    int duration = job->duration > 0 ? job->duration : 40;
    rgb_t color_a = job->color_count > 0 ? job->colors[0] : (rgb_t){0, 0, 0};
    rgb_t color_b = job->color_count > 1 ? job->colors[1] : (rgb_t){255, 255, 255};

    float factor = (float)local_tick / (float)duration;
    if (factor > 1.0f) factor = 1.0f;

    rgb_t color = color_interpolate(color_a, color_b, factor);

    int count = 0;
    for (int i = 0; i < job->target_count && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color;
        count++;
    }

    return count;
}

int pattern_breathe(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                    led_output_t *output, int max_output)
{
    (void)current_colors;

    int duration = job->duration > 0 ? job->duration : 40;
    rgb_t color_a = job->color_count > 0 ? job->colors[0] : (rgb_t){0, 0, 0};
    rgb_t color_b = job->color_count > 1 ? job->colors[1] : (rgb_t){255, 255, 255};

    /* Sine wave oscillation - matches Python: (sin(2*pi*tick/duration)+1)/2,
     * no phase shift (breathe starts at the midpoint, rising). */
    float phase = (float)(local_tick % duration) / (float)duration;
    float factor = (sinf(2.0f * (float)M_PI * phase) + 1.0f) / 2.0f;

    rgb_t color = color_interpolate(color_a, color_b, factor);

    int count = 0;
    for (int i = 0; i < job->target_count && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color;
        count++;
    }

    return count;
}

/**
 * Return the head LED position (as a float) for a wave at the given phase,
 * mirroring PatternMixin._wave_head_position().
 */
static float wave_head_position(int num_leds, int cycle_ticks, int phase, bool reverse)
{
    float position;
    if (cycle_ticks > 1) {
        position = (float)phase * (float)(num_leds - 1) / (float)(cycle_ticks - 1);
    } else {
        position = (float)(num_leds - 1);
    }

    if (reverse) position = (float)(num_leds - 1) - position;
    return position;
}

/**
 * Render one or more wave comets with sub-pixel smoothing, mirroring
 * PatternMixin._render_wave(). Every target LED is written: LEDs not under a
 * head fade the previous frame's color (from current_colors) toward
 * color1/color2 by one step; LEDs under a head (or its sub-pixel neighbor)
 * are brightened toward color2.
 */
static int render_wave(const active_job_t *job, const rgb_t *current_colors,
                        const float *head_positions, int head_count,
                        int width, float ticks_per_led,
                        rgb_t color1, rgb_t color2, bool reverse,
                        led_output_t *output, int max_output)
{
    int target_count = job->target_count;
    if (target_count > max_output) target_count = max_output;

    float fade_ticks = width * ticks_per_led;
    if (fade_ticks < 1.0f) fade_ticks = 1.0f;
    float step_r = ((float)color2.r - (float)color1.r) / fade_ticks;
    float step_g = ((float)color2.g - (float)color1.g) / fade_ticks;
    float step_b = ((float)color2.b - (float)color1.b) / fade_ticks;

    int lo_r = color1.r < color2.r ? color1.r : color2.r;
    int hi_r = color1.r > color2.r ? color1.r : color2.r;
    int lo_g = color1.g < color2.g ? color1.g : color2.g;
    int hi_g = color1.g > color2.g ? color1.g : color2.g;
    int lo_b = color1.b < color2.b ? color1.b : color2.b;
    int hi_b = color1.b > color2.b ? color1.b : color2.b;

    for (int i = 0; i < target_count; i++) {
        float r = (float)current_colors[i].r - step_r;
        float g = (float)current_colors[i].g - step_g;
        float b = (float)current_colors[i].b - step_b;
        if (r < lo_r) r = (float)lo_r;
        if (r > hi_r) r = (float)hi_r;
        if (g < lo_g) g = (float)lo_g;
        if (g > hi_g) g = (float)hi_g;
        if (b < lo_b) b = (float)lo_b;
        if (b > hi_b) b = (float)hi_b;

        output[i].led_index = job->target_indices[i];
        output[i].color = (rgb_t){ (uint8_t)r, (uint8_t)g, (uint8_t)b };
    }

    int fill_count = 1;
    if (ticks_per_led > 0.0f) {
        int fc = (int)ceilf(1.0f / ticks_per_led);
        if (fc > fill_count) fill_count = fc;
    }

    for (int h = 0; h < head_count; h++) {
        float head_position = head_positions[h];
        int head_index;
        float sub_position;

        if (!reverse) {
            head_index = (int)floorf(head_position);
            sub_position = head_position - (float)head_index;
        } else {
            head_index = (int)ceilf(head_position);
            sub_position = (float)head_index - head_position;
        }

        if (head_index < 0 || head_index >= target_count) continue;

        for (int offset = 0; offset < fill_count; offset++) {
            int fill_idx = !reverse ? head_index - offset : head_index + offset;
            if (fill_idx >= 0 && fill_idx < target_count) {
                output[fill_idx].led_index = job->target_indices[fill_idx];
                output[fill_idx].color = color2;
            }
        }

        if (!reverse) {
            if (head_index + 1 < target_count && sub_position > 0.0f) {
                output[head_index + 1].led_index = job->target_indices[head_index + 1];
                output[head_index + 1].color = color_interpolate(color1, color2, sub_position);
            }
        } else {
            if (head_index - 1 >= 0 && sub_position > 0.0f) {
                output[head_index - 1].led_index = job->target_indices[head_index - 1];
                output[head_index - 1].color = color_interpolate(color1, color2, sub_position);
            }
        }
    }

    return target_count;
}

/**
 * Compute Python's `(tick_number - 1) % modulus`, which is always
 * non-negative even when tick_number is 0 (Python's % never returns
 * negative for a positive modulus). local_tick is unsigned, so the
 * subtraction is done in a signed 64-bit domain to avoid wraparound.
 */
static int phase_from_tick_minus_one(uint32_t local_tick, int modulus)
{
    if (modulus <= 0) return 0;
    int64_t signed_tick = (int64_t)local_tick - 1;
    int64_t phase = signed_tick % modulus;
    if (phase < 0) phase += modulus;
    return (int)phase;
}

int pattern_wave(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                 led_output_t *output, int max_output)
{
    int number = job->number > 0 ? job->number : 1;
    int width = job->width > 0 ? job->width : 5;
    bool reverse = job->reverse;
    rgb_t color1 = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    rgb_t color2 = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};

    int num_leds = job->target_count;
    int cycle_ticks = job->duration > 0 ? job->duration : 40;
    int phase = phase_from_tick_minus_one(local_tick, cycle_ticks);
    float ticks_per_led = (float)cycle_ticks / (float)(num_leds > 1 ? num_leds - 1 : 1);

    /* Note: cycle-completion bookkeeping (job->cycles/cycles_completed) is
     * handled centrally in lighting_process_tick() rather than per-pattern
     * like Python's self._count_cycle() call at phase==0. */

    int spacing = number > 0 ? cycle_ticks / number : cycle_ticks;
    if (spacing < 1) spacing = 1;

    int head_count = number > MAX_WAVE_HEADS ? MAX_WAVE_HEADS : number;
    float head_positions[MAX_WAVE_HEADS];
    for (int p = 0; p < head_count; p++) {
        int peak_phase = (phase + p * spacing) % cycle_ticks;
        head_positions[p] = wave_head_position(num_leds, cycle_ticks, peak_phase, reverse);
    }

    return render_wave(job, current_colors, head_positions, head_count, width, ticks_per_led,
                        color1, color2, reverse, output, max_output);
}

int pattern_cylon(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                  led_output_t *output, int max_output)
{
    int width = job->width > 0 ? job->width : 5;
    rgb_t color1 = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    rgb_t color2 = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};

    int num_leds = job->target_count;
    int one_way_ticks = job->duration > 0 ? job->duration : 40;
    int cycle_ticks = one_way_ticks * 2;
    int phase = phase_from_tick_minus_one(local_tick, cycle_ticks);
    float ticks_per_led = (float)one_way_ticks / (float)(num_leds > 1 ? num_leds - 1 : 1);

    bool reverse;
    float head_position;
    if (phase < one_way_ticks) {
        reverse = false;
        head_position = wave_head_position(num_leds, one_way_ticks, phase, false);
    } else {
        reverse = true;
        head_position = wave_head_position(num_leds, one_way_ticks, phase - one_way_ticks, true);
    }

    return render_wave(job, current_colors, &head_position, 1, width, ticks_per_led,
                        color1, color2, reverse, output, max_output);
}

int pattern_phaser_strip(active_job_t *job, uint32_t local_tick, const rgb_t *current_colors,
                         led_output_t *output, int max_output)
{
    int width = job->width > 0 ? job->width : 5;
    rgb_t color1 = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    rgb_t color2 = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};

    int num_leds = job->target_count;
    if (num_leds < 3) return 0;

    int cycle_ticks = job->duration > 0 ? job->duration : 40;
    float ticks_per_led = (float)cycle_ticks / (float)(num_leds - 1);
    int fade_ticks = (int)(width * ticks_per_led);
    if (fade_ticks < 1) fade_ticks = 1;
    int total_ticks = cycle_ticks + fade_ticks;

    int phase = phase_from_tick_minus_one(local_tick, total_ticks);

    /* Known gap vs Python: when this effect finishes (job->cycles reached),
     * Python records a "passthrough" target (the meeting point) so a
     * downstream after/inherit_target effect can continue from it. That
     * scene-entry chaining lives in lighting.c/effects.c and is not
     * implemented here (see report). */
    if (!job->retained_meet_set || phase == 0) {
        int range = num_leds - 2; /* random(1, num_leds - 2) inclusive */
        job->retained_meet_index = 1 + (range > 0 ? (rand() % range) : 0);
        job->retained_meet_set = true;
    }
    int meet_index = job->retained_meet_index;

    if (phase < cycle_ticks) {
        float left_pos, right_pos;
        if (cycle_ticks > 1) {
            left_pos = (float)phase * (float)meet_index / (float)(cycle_ticks - 1);
            right_pos = (float)(num_leds - 1) -
                        (float)phase * (float)(num_leds - 1 - meet_index) / (float)(cycle_ticks - 1);
        } else {
            left_pos = (float)meet_index;
            right_pos = (float)meet_index;
        }

        int n = render_wave(job, current_colors, &left_pos, 1, width, ticks_per_led,
                             color1, color2, false, phaser_left_buf, MAX_LEDS);
        render_wave(job, current_colors, &right_pos, 1, width, ticks_per_led,
                    color1, color2, true, phaser_right_buf, MAX_LEDS);

        if (n > max_output) n = max_output;
        for (int i = 0; i < n; i++) {
            int sum_left = phaser_left_buf[i].color.r + phaser_left_buf[i].color.g + phaser_left_buf[i].color.b;
            int sum_right = phaser_right_buf[i].color.r + phaser_right_buf[i].color.g + phaser_right_buf[i].color.b;
            output[i] = (sum_right > sum_left) ? phaser_right_buf[i] : phaser_left_buf[i];
        }
        return n;
    }

    int fade_phase = phase - cycle_ticks;
    if (fade_phase < fade_ticks - 1) {
        float pos = (float)meet_index;
        return render_wave(job, current_colors, &pos, 1, width, ticks_per_led,
                            color1, color2, false, output, max_output);
    }

    int count = 0;
    for (int i = 0; i < num_leds && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color1;
        count++;
    }
    return count;
}

/* Pattern lookup table */
typedef struct {
    const char *name;
    pattern_fn_t fn;
} pattern_entry_t;

static const pattern_entry_t pattern_table[] = {
    {"solid",        pattern_solid},
    {"blink",        pattern_blink},
    {"pulse",        pattern_pulse},
    {"fade_in",      pattern_fade_in},
    {"breathe",      pattern_breathe},
    {"wave",         pattern_wave},
    {"cylon",        pattern_cylon},
    {"phaser_strip", pattern_phaser_strip},
    {NULL, NULL},
};

pattern_fn_t pattern_get(const char *name)
{
    if (!name) return pattern_solid;

    for (int i = 0; pattern_table[i].name != NULL; i++) {
        if (strcmp(name, pattern_table[i].name) == 0) {
            return pattern_table[i].fn;
        }
    }

    ESP_LOGW(TAG, "Unknown pattern: %s, defaulting to solid", name);
    return pattern_solid;
}

const char **pattern_get_names(int *count)
{
    if (count) *count = pattern_count;
    return pattern_names;
}
