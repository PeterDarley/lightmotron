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

int pattern_solid(const active_job_t *job, uint32_t local_tick,
                  led_output_t *output, int max_output)
{
    rgb_t color = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    int count = 0;

    for (int i = 0; i < job->target_count && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color;
        count++;
    }

    return count;
}

int pattern_blink(const active_job_t *job, uint32_t local_tick,
                  led_output_t *output, int max_output)
{
    int duration = job->duration > 0 ? job->duration : 20;
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

int pattern_pulse(const active_job_t *job, uint32_t local_tick,
                  led_output_t *output, int max_output)
{
    int duration = job->duration > 0 ? job->duration : 10;
    int period = job->period > 0 ? job->period : duration * 4;
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

int pattern_fade_in(const active_job_t *job, uint32_t local_tick,
                    led_output_t *output, int max_output)
{
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

int pattern_breathe(const active_job_t *job, uint32_t local_tick,
                    led_output_t *output, int max_output)
{
    int duration = job->duration > 0 ? job->duration : 60;
    rgb_t color_a = job->color_count > 0 ? job->colors[0] : (rgb_t){0, 0, 0};
    rgb_t color_b = job->color_count > 1 ? job->colors[1] : (rgb_t){255, 255, 255};

    /* Sine wave oscillation */
    float phase = (float)(local_tick % duration) / (float)duration;
    float factor = (sinf(phase * 2.0f * M_PI - M_PI / 2.0f) + 1.0f) / 2.0f;

    rgb_t color = color_interpolate(color_a, color_b, factor);

    int count = 0;
    for (int i = 0; i < job->target_count && count < max_output; i++) {
        output[count].led_index = job->target_indices[i];
        output[count].color = color;
        count++;
    }

    return count;
}

int pattern_wave(const active_job_t *job, uint32_t local_tick,
                 led_output_t *output, int max_output)
{
    int num_waves = job->number > 0 ? job->number : 1;
    int width = job->width > 0 ? job->width : 5;
    int duration = job->duration > 0 ? job->duration : 40;
    bool reverse = job->reverse;

    rgb_t wave_color = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 255, 255};
    rgb_t bg_color = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};

    int target_count = job->target_count;
    float speed = (float)target_count / (float)duration;
    float position = (float)local_tick * speed;

    /* Wrap position */
    int total_travel = target_count + width;
    int pos = (int)position % total_travel;
    if (reverse) {
        pos = total_travel - 1 - pos;
    }

    int count = 0;
    for (int i = 0; i < target_count && count < max_output; i++) {
        rgb_t pixel_color = bg_color;

        /* Check each wave */
        for (int w = 0; w < num_waves; w++) {
            int wave_offset = (w * target_count) / num_waves;
            int wave_pos = (pos + wave_offset) % total_travel;
            int distance = i - (wave_pos - width);

            if (distance >= 0 && distance < width) {
                float intensity = 1.0f - ((float)distance / (float)width);
                pixel_color = color_interpolate(bg_color, wave_color, intensity);
            }
        }

        output[count].led_index = job->target_indices[i];
        output[count].color = pixel_color;
        count++;
    }

    return count;
}

int pattern_cylon(const active_job_t *job, uint32_t local_tick,
                  led_output_t *output, int max_output)
{
    int width = job->width > 0 ? job->width : 5;
    int duration = job->duration > 0 ? job->duration : 40;

    rgb_t wave_color = job->color_count > 0 ? job->colors[0] : (rgb_t){255, 0, 0};
    rgb_t bg_color = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};

    int target_count = job->target_count;
    int travel = target_count - width;
    if (travel < 1) travel = 1;

    /* Bounce: position goes 0 → travel → 0 */
    int cycle_len = travel * 2;
    float speed = (float)cycle_len / (float)duration;
    int raw_pos = (int)((float)local_tick * speed) % cycle_len;
    int pos = raw_pos <= travel ? raw_pos : cycle_len - raw_pos;

    int count = 0;
    for (int i = 0; i < target_count && count < max_output; i++) {
        rgb_t pixel_color = bg_color;
        int distance = i - pos;
        if (distance >= 0 && distance < width) {
            float intensity = 1.0f - ((float)distance / (float)width);
            pixel_color = color_interpolate(bg_color, wave_color, intensity);
        }

        output[count].led_index = job->target_indices[i];
        output[count].color = pixel_color;
        count++;
    }

    return count;
}

int pattern_phaser_strip(const active_job_t *job, uint32_t local_tick,
                         led_output_t *output, int max_output)
{
    int width = job->width > 0 ? job->width : 5;
    int duration = job->duration > 0 ? job->duration : 40;

    rgb_t wave_color = job->color_count > 0 ? job->colors[0] : (rgb_t){0, 0, 255};
    rgb_t bg_color = job->color_count > 1 ? job->colors[1] : (rgb_t){0, 0, 0};

    int target_count = job->target_count;
    int half = target_count / 2;
    float speed = (float)half / (float)duration;
    int pos = (int)((float)local_tick * speed) % (half + width);

    int count = 0;
    for (int i = 0; i < target_count && count < max_output; i++) {
        rgb_t pixel_color = bg_color;

        /* Wave from left */
        int dist_left = i - (pos - width);
        if (i < half && dist_left >= 0 && dist_left < width) {
            float intensity = 1.0f - ((float)dist_left / (float)width);
            pixel_color = color_interpolate(bg_color, wave_color, intensity);
        }

        /* Wave from right */
        int right_pos = target_count - 1 - pos;
        int dist_right = right_pos - i;
        if (i >= half && dist_right >= 0 && dist_right < width) {
            float intensity = 1.0f - ((float)dist_right / (float)width);
            pixel_color = color_interpolate(bg_color, wave_color, intensity);
        }

        output[count].led_index = job->target_indices[i];
        output[count].color = pixel_color;
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
