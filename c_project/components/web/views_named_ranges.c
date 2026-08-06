#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "leds.h"
#include "animation.h"
#include "json_helpers.h"
#include "cJSON.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------------
 * Server-side LED-picker selection state.
 *
 * Mirrors the module-level _selected_leds / _editing_range_name globals in
 * web/views_named_ranges.py: a single shared in-progress selection for the
 * (single-user) LED naming tool. Each item is either a cJSON number (a
 * direct LED index) or a cJSON string of the form "named:<range>" (an
 * included subrange).
 * ------------------------------------------------------------------- */
static cJSON *g_selected_leds = NULL;         /* array, lazily created */
static char g_editing_range_name[64] = "";
static bool g_has_editing_range_name = false;

static cJSON *selected_leds(void)
{
    if (!g_selected_leds) g_selected_leds = cJSON_CreateArray();
    return g_selected_leds;
}

static void set_selected_leds(cJSON *tokens)
{
    if (g_selected_leds) cJSON_Delete(g_selected_leds);
    g_selected_leds = tokens ? tokens : cJSON_CreateArray();
}

/* Transient queue for the "Reorder Strings" tool: names of ranges the user
 * has chosen to reorder. Mirrors _reorder_queue in views_named_ranges.py.
 * Display/swap order is always recomputed from each range's current
 * physical start index, not insertion order into this array. */
#define MAX_REORDER_QUEUE 32
static char g_reorder_queue[MAX_REORDER_QUEUE][64];
static int g_reorder_queue_count = 0;

static bool reorder_queue_contains(const char *name)
{
    for (int i = 0; i < g_reorder_queue_count; i++) {
        if (strcmp(g_reorder_queue[i], name) == 0) return true;
    }
    return false;
}

static void reorder_queue_remove(const char *name)
{
    for (int i = 0; i < g_reorder_queue_count; i++) {
        if (strcmp(g_reorder_queue[i], name) == 0) {
            for (int j = i; j < g_reorder_queue_count - 1; j++) {
                strncpy(g_reorder_queue[j], g_reorder_queue[j + 1], sizeof(g_reorder_queue[j]) - 1);
                g_reorder_queue[j][sizeof(g_reorder_queue[j]) - 1] = '\0';
            }
            g_reorder_queue_count--;
            return;
        }
    }
}

static bool reorder_queue_add(const char *name)
{
    if (g_reorder_queue_count >= MAX_REORDER_QUEUE || reorder_queue_contains(name)) return false;
    strncpy(g_reorder_queue[g_reorder_queue_count], name, sizeof(g_reorder_queue[0]) - 1);
    g_reorder_queue[g_reorder_queue_count][sizeof(g_reorder_queue[0]) - 1] = '\0';
    g_reorder_queue_count++;
    return true;
}

/* Mirrors _parse_selected_tokens(): parse a comma-separated string of ints
 * and "named:X" tokens into a cJSON array of numbers/strings. */
static cJSON *parse_selected_tokens(const char *csv)
{
    cJSON *out = cJSON_CreateArray();
    if (!csv || !csv[0]) return out;

    char *copy = strdup(csv);
    if (!copy) return out;

    char *token = strtok(copy, ",");
    while (token) {
        while (*token == ' ') token++;
        size_t tlen = strlen(token);
        while (tlen > 0 && token[tlen - 1] == ' ') token[--tlen] = '\0';

        if (tlen > 0) {
            if (strncmp(token, "named:", 6) == 0) {
                if (token[6] != '\0') cJSON_AddItemToArray(out, cJSON_CreateString(token));
            } else {
                char *endptr;
                long v = strtol(token, &endptr, 10);
                if (endptr != token && *endptr == '\0') cJSON_AddItemToArray(out, cJSON_CreateNumber((double)v));
            }
        }
        token = strtok(NULL, ",");
    }
    free(copy);
    return out;
}

/* Joins a token array (numbers / "named:X" strings) into a single
 * comma-separated spec string consumable by lighting_get_targets()/
 * target_spec_resolve(), which already supports mixed "N,named:X,N" specs
 * with cross-token dedup - equivalent to Lighting.get_targets() called on
 * a list. Caller must free() the result. */
static char *tokens_to_spec(cJSON *tokens)
{
    if (!tokens) return strdup("");

    size_t cap = 256;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    size_t len = 0;

    for (cJSON *item = tokens->child; item; item = item->next) {
        char piece[80];
        if (cJSON_IsNumber(item)) {
            snprintf(piece, sizeof(piece), "%d", item->valueint);
        } else if (cJSON_IsString(item) && item->valuestring) {
            snprintf(piece, sizeof(piece), "%s", item->valuestring);
        } else {
            continue;
        }
        size_t piece_len = strlen(piece);
        size_t needed = len + piece_len + 2;
        if (needed > cap) {
            cap = needed * 2;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        if (len > 0) { buf[len++] = ','; buf[len] = '\0'; }
        memcpy(buf + len, piece, piece_len + 1);
        len += piece_len;
    }
    return buf;
}

#define MAX_RESOLVED_LEDS 512

/* Resolves a token array to a deduplicated LED index array via
 * lighting_get_targets(). Returns the number of indices written. */
static int resolve_tokens(cJSON *tokens, int *indices, int max_indices)
{
    char *spec = tokens_to_spec(tokens);
    if (!spec) return 0;
    int count = (spec[0] != '\0') ? lighting_get_targets(spec, indices, max_indices) : 0;
    free(spec);
    return count;
}

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

/* Mirrors summarize_led_list(): compact human-readable summary of an LED
 * index set. Caller must free() the result. */
static char *summarize_led_list(const int *led_indices, int count)
{
    if (count <= 0) return strdup("none");

    int *sorted = malloc(sizeof(int) * count);
    memcpy(sorted, led_indices, sizeof(int) * count);
    qsort(sorted, count, sizeof(int), cmp_int);

    /* De-dup in place. */
    int unique_count = 0;
    for (int i = 0; i < count; i++) {
        if (unique_count == 0 || sorted[i] != sorted[unique_count - 1]) sorted[unique_count++] = sorted[i];
    }

    typedef struct { int start, end; } segment_t;
    segment_t *segments = malloc(sizeof(segment_t) * unique_count);
    int seg_count = 0;
    int start = sorted[0], end = sorted[0];
    for (int i = 1; i < unique_count; i++) {
        if (sorted[i] == end + 1) {
            end = sorted[i];
        } else {
            segments[seg_count].start = start;
            segments[seg_count].end = end;
            seg_count++;
            start = sorted[i];
            end = sorted[i];
        }
    }
    segments[seg_count].start = start;
    segments[seg_count].end = end;
    seg_count++;

    char *result = malloc(128);
    if (seg_count == 1) {
        if (segments[0].start == segments[0].end) snprintf(result, 128, "%d", segments[0].start);
        else snprintf(result, 128, "%d-%d", segments[0].start, segments[0].end);
    } else if (seg_count == 2) {
        char a[32], b[32];
        if (segments[0].start == segments[0].end) snprintf(a, sizeof(a), "%d", segments[0].start);
        else snprintf(a, sizeof(a), "%d-%d", segments[0].start, segments[0].end);
        if (segments[1].start == segments[1].end) snprintf(b, sizeof(b), "%d", segments[1].start);
        else snprintf(b, sizeof(b), "%d-%d", segments[1].start, segments[1].end);
        snprintf(result, 128, "%s, %s", a, b);
    } else {
        snprintf(result, 128, "%d to %d", sorted[0], sorted[unique_count - 1]);
    }

    free(segments);
    free(sorted);
    return result;
}

/* Mirrors _build_named_range_entries(): sorted list of {name, summary,
 * min_led} for every named range on the current model. */
static cJSON *build_named_range_entries(const char *sort_by)
{
    cJSON *result = cJSON_CreateArray();
    cJSON *model = lighting_get_settings();
    cJSON *named_ranges = model ? cJSON_GetObjectItem(model, "named_ranges") : NULL;
    if (!named_ranges) return result;

    int total = cJSON_GetArraySize(named_ranges);
    if (total <= 0) return result;

    const char **names = calloc(total, sizeof(char *));
    int count = 0;
    for (cJSON *item = named_ranges->child; item; item = item->next) names[count++] = item->string;

    for (int i = 0; i < count; i++) {
        cJSON *members = cJSON_GetObjectItem(named_ranges, names[i]);
        int indices[MAX_RESOLVED_LEDS];
        int idx_count = resolve_tokens(members, indices, MAX_RESOLVED_LEDS);

        int min_led = 99999;
        for (int j = 0; j < idx_count; j++) if (indices[j] < min_led) min_led = indices[j];

        char *summary = summarize_led_list(indices, idx_count);

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", names[i]);
        cJSON_AddStringToObject(entry, "summary", summary);
        cJSON_AddNumberToObject(entry, "min_led", min_led);
        cJSON_AddItemToArray(result, entry);
        free(summary);
    }
    free(names);

    if (sort_by && strcmp(sort_by, "leds") == 0) {
        /* Simple insertion sort by min_led - entry counts are small. */
        int n = cJSON_GetArraySize(result);
        for (int i = 1; i < n; i++) {
            cJSON *item = cJSON_DetachItemFromArray(result, i);
            int val = json_get_int(item, "min_led", 99999);
            int j = i - 1;
            while (j >= 0 && json_get_int(cJSON_GetArrayItem(result, j), "min_led", 99999) > val) j--;
            cJSON_InsertItemInArray(result, j + 1, item);
        }
    } else {
        int n = cJSON_GetArraySize(result);
        for (int i = 1; i < n; i++) {
            cJSON *item = cJSON_DetachItemFromArray(result, i);
            const char *name = json_get_string(item, "name", "");
            int j = i - 1;
            while (j >= 0 && strcasecmp(json_get_string(cJSON_GetArrayItem(result, j), "name", ""), name) > 0) j--;
            cJSON_InsertItemInArray(result, j + 1, item);
        }
    }

    return result;
}

typedef struct {
    const char **names;
    cJSON **members; /* member list per node, index-aligned with names */
    int count;
    bool *visited;
    bool *recstack;
} cycle_graph_t;

static int graph_find_index(cycle_graph_t *g, const char *name)
{
    for (int i = 0; i < g->count; i++) {
        if (strcmp(g->names[i], name) == 0) return i;
    }
    return -1;
}

static bool graph_dfs(cycle_graph_t *g, int node)
{
    if (g->recstack[node]) return true;
    if (g->visited[node]) return false;

    g->visited[node] = true;
    g->recstack[node] = true;

    cJSON *members = g->members[node];
    if (members && cJSON_IsArray(members)) {
        for (cJSON *item = members->child; item; item = item->next) {
            if (cJSON_IsString(item) && item->valuestring && strncmp(item->valuestring, "named:", 6) == 0) {
                int child = graph_find_index(g, item->valuestring + 6);
                if (child >= 0 && graph_dfs(g, child)) return true;
            }
        }
    }

    g->recstack[node] = false;
    return false;
}

/* Detects a cycle in the named_ranges graph after hypothetically setting
 * `candidate_name` to `candidate_members`. Mirrors _named_ranges_has_cycle()
 * applied to the Python "candidate" dict (existing ranges + the pending
 * save). Only "named:X" references form graph edges. */
static bool has_cycle(cJSON *named_ranges, const char *candidate_name, cJSON *candidate_members)
{
    int total = named_ranges ? cJSON_GetArraySize(named_ranges) : 0;
    int max_nodes = total + 1; /* +1 in case candidate_name is new */
    const char **names = calloc(max_nodes, sizeof(char *));
    cJSON **members = calloc(max_nodes, sizeof(cJSON *));
    int n = 0;

    if (named_ranges) {
        for (cJSON *item = named_ranges->child; item; item = item->next) {
            if (strcmp(item->string, candidate_name) == 0) continue; /* superseded by candidate below */
            names[n] = item->string;
            members[n] = item;
            n++;
        }
    }
    names[n] = candidate_name;
    members[n] = candidate_members;
    n++;

    cycle_graph_t g = {
        .names = names,
        .members = members,
        .count = n,
        .visited = calloc(n, sizeof(bool)),
        .recstack = calloc(n, sizeof(bool)),
    };

    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        if (!g.visited[i] && graph_dfs(&g, i)) found = true;
    }

    free(g.visited);
    free(g.recstack);
    free(names);
    free(members);
    return found;
}

/* Mirrors _get_led_picker_script_version(): a cache-busting version string
 * derived from the script's mtime on disk, or "0" if it can't be stat'd. */
static char *led_picker_script_version(void)
{
    struct stat st;
    if (stat("/spiffs/www/scripts/led_picker.js", &st) == 0) {
        char *out = malloc(32);
        snprintf(out, 32, "%ld", (long)st.st_mtime);
        return out;
    }
    return strdup("0");
}

/* Builds the LED picker's led_list: one {index, css_class} entry per
 * configured LED, highlighting only *directly* selected indices (not LEDs
 * only reachable via an included subrange) - mirrors _named_range_context()'s
 * direct_selected_leds/led_list construction. */
static cJSON *build_led_list(cJSON *tokens)
{
    int total = leds_total_count();
    cJSON *list = cJSON_CreateArray();

    for (int i = 0; i < total; i++) {
        bool selected = false;
        for (cJSON *item = tokens->child; item; item = item->next) {
            if (cJSON_IsNumber(item) && item->valueint == i) { selected = true; break; }
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "index", i);
        cJSON_AddStringToObject(entry, "css_class", selected ? "btn-warning" : "btn-outline-secondary");
        cJSON_AddItemToArray(list, entry);
    }
    return list;
}

/* Mirrors _named_range_context(): full LED-picker template context built
 * from the current server-side selection state. */
static cJSON *build_named_range_context(const char *sort_by)
{
    cJSON *tokens = selected_leds();

    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "led_list", build_led_list(tokens));
    cJSON_AddItemToObject(ctx, "named_range_names", build_named_range_entries(sort_by));

    if (g_has_editing_range_name) cJSON_AddStringToObject(ctx, "range_name", g_editing_range_name);
    else cJSON_AddNullToObject(ctx, "range_name");

    const char *effective_sort = (sort_by && (strcmp(sort_by, "name") == 0 || strcmp(sort_by, "leds") == 0))
                                      ? sort_by
                                      : "name";
    cJSON_AddStringToObject(ctx, "sort_by", effective_sort);

    cJSON *selected_subranges = cJSON_CreateArray();
    for (cJSON *item = tokens->child; item; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring && strncmp(item->valuestring, "named:", 6) == 0) {
            const char *subrange_name = item->valuestring + 6;
            bool dup = false;
            for (cJSON *s = selected_subranges->child; s; s = s->next) {
                if (strcmp(s->valuestring, subrange_name) == 0) { dup = true; break; }
            }
            if (!dup && subrange_name[0]) cJSON_AddItemToArray(selected_subranges, cJSON_CreateString(subrange_name));
        }
    }
    cJSON_AddItemToObject(ctx, "selected_subranges", selected_subranges);

    char *spec = tokens_to_spec(tokens);
    cJSON_AddStringToObject(ctx, "selected_tokens_str", spec ? spec : "");
    free(spec);

    int indices[MAX_RESOLVED_LEDS];
    int idx_count = resolve_tokens(tokens, indices, MAX_RESOLVED_LEDS);
    char *led_summary = summarize_led_list(indices, idx_count);
    cJSON_AddStringToObject(ctx, "led_summary", led_summary);
    free(led_summary);

    char *version = led_picker_script_version();
    cJSON_AddStringToObject(ctx, "led_picker_script_version", version);
    free(version);

    return ctx;
}

/* Lights up the currently-selected LEDs (identify) or clears them, matching
 * the show-selection side effect at the end of NamedRangeView.get/post and
 * NamedRangeSetView/NamedRangeRemoveSubrangeView. */
static void apply_led_identify(cJSON *tokens)
{
    int indices[MAX_RESOLVED_LEDS];
    int idx_count = resolve_tokens(tokens, indices, MAX_RESOLVED_LEDS);
    if (idx_count > 0) {
        leds_identify(indices, idx_count);
    } else {
        leds_clear();
        leds_show();
    }
}

/* Mirrors _rename_named_range_refs(): updates scene job "target" fields and
 * other named ranges' "named:X" member references when a range is renamed. */
static void rename_named_range_refs(cJSON *model, const char *old_name, const char *new_name)
{
    char old_ref[128], new_ref[128];
    snprintf(old_ref, sizeof(old_ref), "named:%s", old_name);
    snprintf(new_ref, sizeof(new_ref), "named:%s", new_name);

    cJSON *scenes = model ? cJSON_GetObjectItem(model, "scenes") : NULL;
    if (scenes) {
        for (cJSON *scene = scenes->child; scene; scene = scene->next) {
            for (cJSON *entry = scene->child; entry; entry = entry->next) {
                cJSON *target = cJSON_GetObjectItem(entry, "target");
                if (target && cJSON_IsString(target) && target->valuestring &&
                    strcmp(target->valuestring, old_ref) == 0) {
                    cJSON_ReplaceItemInObject(entry, "target", cJSON_CreateString(new_ref));
                }
            }
        }
    }

    cJSON *named_ranges = model ? cJSON_GetObjectItem(model, "named_ranges") : NULL;
    if (named_ranges) {
        for (cJSON *range = named_ranges->child; range; range = range->next) {
            if (!cJSON_IsArray(range)) continue;
            int idx = 0;
            cJSON *item = range->child;
            while (item) {
                cJSON *next = item->next;
                if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, old_ref) == 0) {
                    cJSON_ReplaceItemInArray(range, idx, cJSON_CreateString(new_ref));
                }
                item = next;
                idx++;
            }
        }
    }
}

/* ---------------------------------------------------------------------
 * Reorder Strings tool: swap primary, contiguous named ranges' physical
 * positions, rewriting every affected LED index reference. Mirrors
 * _is_primary_contiguous / _reorder_eligible_ranges / _compute_swap_permutation
 * / _swap_named_ranges / _reindex_leds in web/views_named_ranges.py and
 * web/views_common.py.
 * ------------------------------------------------------------------- */

/* True when every member is a raw LED index (no "named:" references) and the
 * members form one unbroken contiguous run with no gaps or duplicates. */
static bool is_primary_contiguous(cJSON *members)
{
    if (!members || !cJSON_IsArray(members)) return false;

    int values[MAX_LEDS];
    int n = 0;
    for (cJSON *item = members->child; item; item = item->next) {
        if (!cJSON_IsNumber(item) || n >= MAX_LEDS) return false;
        values[n++] = item->valueint;
    }
    if (n == 0) return false;

    qsort(values, n, sizeof(int), cmp_int);
    for (int i = 1; i < n; i++) {
        if (values[i] == values[i - 1]) return false; /* duplicate */
        if (values[i] != values[i - 1] + 1) return false; /* gap */
    }
    return true;
}

/* Primary+contiguous named ranges eligible for the reorder tool, sorted by
 * current physical start index, as {name, members (sorted numbers), start,
 * len}. A range extending past the current LED count is excluded (stale
 * range from a hardware reconfiguration). Mirrors _reorder_eligible_ranges(). */
static cJSON *reorder_eligible_ranges(void)
{
    cJSON *result = cJSON_CreateArray();
    cJSON *model = lighting_get_settings();
    cJSON *named_ranges = model ? cJSON_GetObjectItem(model, "named_ranges") : NULL;
    if (!named_ranges) return result;

    int led_count = leds_total_count();

    for (cJSON *range = named_ranges->child; range; range = range->next) {
        if (!is_primary_contiguous(range)) continue;

        int n = cJSON_GetArraySize(range);
        int values[MAX_LEDS];
        int idx = 0;
        for (cJSON *item = range->child; item; item = item->next) values[idx++] = item->valueint;
        qsort(values, n, sizeof(int), cmp_int);

        if (values[n - 1] >= led_count) continue;

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", range->string);
        cJSON *members_arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(members_arr, cJSON_CreateNumber(values[i]));
        cJSON_AddItemToObject(entry, "members", members_arr);
        cJSON_AddNumberToObject(entry, "start", values[0]);
        cJSON_AddNumberToObject(entry, "len", n);
        cJSON_AddItemToArray(result, entry);
    }

    /* Insertion sort by start - mirrors build_named_range_entries()'s pattern. */
    int total = cJSON_GetArraySize(result);
    for (int i = 1; i < total; i++) {
        cJSON *item = cJSON_DetachItemFromArray(result, i);
        int val = json_get_int(item, "start", 0);
        int j = i - 1;
        while (j >= 0 && json_get_int(cJSON_GetArrayItem(result, j), "start", 0) > val) j--;
        cJSON_InsertItemInArray(result, j + 1, item);
    }

    return result;
}

/* old_index -> new_index map, bounded by MAX_LEDS entries (a permutation
 * only ever touches indices, each unique, so it can never exceed the total
 * LED count). Linear lookup is fine here: this runs once per user click, not
 * on any hot path. */
typedef struct {
    int old_index[MAX_LEDS];
    int new_index[MAX_LEDS];
    int count;
} permutation_t;

static int permutation_lookup(const permutation_t *p, int old_index)
{
    for (int i = 0; i < p->count; i++) {
        if (p->old_index[i] == old_index) return p->new_index[i];
    }
    return old_index; /* identity outside the swap's own span */
}

static void permutation_add(permutation_t *p, int old_index, int new_index)
{
    if (p->count < MAX_LEDS) {
        p->old_index[p->count] = old_index;
        p->new_index[p->count] = new_index;
        p->count++;
    }
}

/* Builds the old_index -> new_index permutation for swapping two
 * non-overlapping contiguous ranges' physical positions. Whichever range
 * starts first is treated as "A"; any content strictly between them shifts
 * by the two ranges' length difference so it keeps its own relative
 * position. Each of A, B, and the shifted middle content keeps its own
 * internal LED order - load-bearing, since pattern rendering (wave, cylon,
 * gradient, etc.) derives visual position from order within a resolved
 * target list, not raw index value. Mirrors _compute_swap_permutation(). */
static void compute_swap_permutation(cJSON *members_a_in, cJSON *members_b_in, permutation_t *out)
{
    out->count = 0;

    int a_vals[MAX_LEDS], b_vals[MAX_LEDS];
    int a_n = 0, b_n = 0;
    for (cJSON *item = members_a_in->child; item; item = item->next) a_vals[a_n++] = item->valueint;
    for (cJSON *item = members_b_in->child; item; item = item->next) b_vals[b_n++] = item->valueint;
    qsort(a_vals, a_n, sizeof(int), cmp_int);
    qsort(b_vals, b_n, sizeof(int), cmp_int);

    int *a = a_vals, *b = b_vals;
    int an = a_n, bn = b_n;
    if (a[0] > b[0]) {
        int *tmp_p = a; a = b; b = tmp_p;
        int tmp_n = an; an = bn; bn = tmp_n;
    }

    int a_start = a[0], a_len = an;
    int b_start = b[0], b_len = bn;

    for (int k = 0; k < bn; k++) {
        permutation_add(out, b[k], a_start + k);
    }

    int middle_start = a_start + a_len;
    for (int old_index = middle_start; old_index < b_start; old_index++) {
        permutation_add(out, old_index, old_index + (b_len - a_len));
    }

    int a_new_start = b_start + b_len - a_len;
    for (int k = 0; k < an; k++) {
        permutation_add(out, a[k], a_new_start + k);
    }
}

/* Remaps every raw LED index reference in the active model per `p`. Mirrors
 * _reindex_leds(): named-range members are remapped elementwise in place;
 * scene "target" strings ("all"/"named:X" left untouched) are resolved to
 * their ordered index list, remapped PRESERVING that order (not sorted --
 * sorting would scramble wave/gradient/rainbow visual direction, since those
 * patterns derive position from order within the target, not index value),
 * then re-serialized as a single digit, a compact "a-b" range if still
 * ascending-contiguous, or a comma-joined string otherwise. */
static void reindex_leds(const permutation_t *p, cJSON *model)
{
    cJSON *named_ranges = model ? cJSON_GetObjectItem(model, "named_ranges") : NULL;
    if (named_ranges) {
        for (cJSON *range = named_ranges->child; range; range = range->next) {
            if (!cJSON_IsArray(range)) continue;
            for (cJSON *item = range->child; item; item = item->next) {
                if (cJSON_IsNumber(item)) {
                    cJSON_SetNumberValue(item, permutation_lookup(p, item->valueint));
                }
            }
        }
    }

    cJSON *scenes = model ? cJSON_GetObjectItem(model, "scenes") : NULL;
    if (!scenes) return;

    for (cJSON *scene = scenes->child; scene; scene = scene->next) {
        for (cJSON *entry = scene->child; entry; entry = entry->next) {
            cJSON *target = cJSON_GetObjectItem(entry, "target");
            if (!target || !cJSON_IsString(target) || !target->valuestring) continue;
            const char *val = target->valuestring;
            if (strcmp(val, "all") == 0 || strncmp(val, "named:", 6) == 0) continue;

            int indices[MAX_RESOLVED_LEDS];
            int count = lighting_get_targets(val, indices, MAX_RESOLVED_LEDS);
            if (count <= 0) continue;

            int remapped[MAX_RESOLVED_LEDS];
            for (int i = 0; i < count; i++) remapped[i] = permutation_lookup(p, indices[i]);

            char new_target[512];
            if (count == 1) {
                snprintf(new_target, sizeof(new_target), "%d", remapped[0]);
            } else {
                bool contiguous_ascending = true;
                for (int i = 1; i < count; i++) {
                    if (remapped[i] != remapped[i - 1] + 1) { contiguous_ascending = false; break; }
                }
                if (contiguous_ascending) {
                    snprintf(new_target, sizeof(new_target), "%d-%d", remapped[0], remapped[count - 1]);
                } else {
                    size_t off = 0;
                    new_target[0] = '\0';
                    for (int i = 0; i < count && off < sizeof(new_target) - 16; i++) {
                        int written = snprintf(new_target + off, sizeof(new_target) - off,
                                                i == 0 ? "%d" : ",%d", remapped[i]);
                        if (written > 0) off += (size_t)written;
                    }
                }
            }
            cJSON_ReplaceItemInObject(entry, "target", cJSON_CreateString(new_target));
        }
    }
}

/* Swaps two named ranges' physical LED positions, rewriting every affected
 * reference. Re-validates both ranges are still eligible and non-overlapping
 * against current storage before acting, since the reorder queue is
 * transient per-process state that nothing resets on a model switch.
 * Returns false (no-op) if either range is no longer valid for this.
 * Mirrors _swap_named_ranges(). */
static bool swap_named_ranges(const char *name_a, const char *name_b)
{
    cJSON *model = lighting_get_settings();
    cJSON *named_ranges = model ? cJSON_GetObjectItem(model, "named_ranges") : NULL;
    if (!named_ranges) return false;

    cJSON *members_a = cJSON_GetObjectItem(named_ranges, name_a);
    cJSON *members_b = cJSON_GetObjectItem(named_ranges, name_b);
    if (!is_primary_contiguous(members_a) || !is_primary_contiguous(members_b)) return false;

    for (cJSON *ia = members_a->child; ia; ia = ia->next) {
        for (cJSON *ib = members_b->child; ib; ib = ib->next) {
            if (ia->valueint == ib->valueint) return false; /* overlap */
        }
    }

    permutation_t permutation;
    compute_swap_permutation(members_a, members_b, &permutation);
    reindex_leds(&permutation, model);

    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    persistent_dict_mark_dirty(store); persistent_dict_save(store);
    return true;
}

/* Builds the reorder tool's template context: eligible ranges split into
 * "queued" (in current physical order) and "available" to add, each with a
 * summarize_led_list() summary attached. Mirrors _reorder_context(). */
static cJSON *build_reorder_context(void)
{
    cJSON *eligible = reorder_eligible_ranges();

    for (cJSON *entry = eligible->child; entry; entry = entry->next) {
        cJSON *members = cJSON_GetObjectItem(entry, "members");
        int values[MAX_LEDS];
        int n = 0;
        for (cJSON *item = members->child; item; item = item->next) values[n++] = item->valueint;
        char *summary = summarize_led_list(values, n);
        cJSON_AddStringToObject(entry, "summary", summary);
        free(summary);
    }

    cJSON *queued = cJSON_CreateArray();
    cJSON *available = cJSON_CreateArray();
    for (cJSON *entry = eligible->child; entry; entry = entry->next) {
        const char *name = json_get_string(entry, "name", "");
        if (reorder_queue_contains(name)) {
            cJSON_AddItemToArray(queued, cJSON_Duplicate(entry, 1));
        } else {
            cJSON_AddItemToArray(available, cJSON_Duplicate(entry, 1));
        }
    }
    cJSON_Delete(eligible);

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "page_title", "Named Ranges");
    cJSON_AddItemToObject(ctx, "reorder_queue", queued);
    cJSON_AddItemToObject(ctx, "reorder_available", available);
    return ctx;
}

http_response_t *view_named_range_reorder(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        g_reorder_queue_count = 0;
        return webserver_render_response("setup/named_ranges_reorder.html", build_reorder_context());
    }

    const char *action = request_get_form_field(req, "action");
    const char *range_name = request_get_form_field(req, "range_name");
    const char *error = NULL;

    /* Drop any queued range no longer eligible before acting, rather than
     * acting on stale data. */
    cJSON *eligible_now = reorder_eligible_ranges();
    for (int i = g_reorder_queue_count - 1; i >= 0; i--) {
        bool still_eligible = false;
        for (cJSON *entry = eligible_now->child; entry; entry = entry->next) {
            if (strcmp(json_get_string(entry, "name", ""), g_reorder_queue[i]) == 0) {
                still_eligible = true;
                break;
            }
        }
        if (!still_eligible) reorder_queue_remove(g_reorder_queue[i]);
    }

    if (action && strcmp(action, "add_to_reorder_list") == 0 && range_name && range_name[0]) {
        cJSON *candidate = NULL;
        for (cJSON *entry = eligible_now->child; entry; entry = entry->next) {
            if (strcmp(json_get_string(entry, "name", ""), range_name) == 0) { candidate = entry; break; }
        }
        if (!candidate) {
            error = "That range is no longer eligible to reorder.";
        } else if (!reorder_queue_contains(range_name)) {
            bool overlaps = false;
            cJSON *cand_members = cJSON_GetObjectItem(candidate, "members");
            for (int i = 0; i < g_reorder_queue_count && !overlaps; i++) {
                cJSON *other = NULL;
                for (cJSON *entry = eligible_now->child; entry; entry = entry->next) {
                    if (strcmp(json_get_string(entry, "name", ""), g_reorder_queue[i]) == 0) { other = entry; break; }
                }
                if (!other) continue;
                cJSON *other_members = cJSON_GetObjectItem(other, "members");
                for (cJSON *ca = cand_members->child; ca && !overlaps; ca = ca->next) {
                    for (cJSON *cb = other_members->child; cb; cb = cb->next) {
                        if (ca->valueint == cb->valueint) { overlaps = true; break; }
                    }
                }
            }
            if (overlaps) {
                error = "That range overlaps a range already in the list.";
            } else {
                reorder_queue_add(range_name);
            }
        }
    } else if (action && strcmp(action, "remove_from_reorder_list") == 0 && range_name && range_name[0]) {
        reorder_queue_remove(range_name);
    } else if (action && range_name && range_name[0] &&
               (strcmp(action, "move_range_up") == 0 || strcmp(action, "move_range_down") == 0)) {
        char ordered_names[MAX_REORDER_QUEUE][64];
        int ordered_count = 0;
        for (cJSON *entry = eligible_now->child; entry && ordered_count < MAX_REORDER_QUEUE; entry = entry->next) {
            const char *name = json_get_string(entry, "name", "");
            if (reorder_queue_contains(name)) {
                strncpy(ordered_names[ordered_count], name, sizeof(ordered_names[0]) - 1);
                ordered_names[ordered_count][sizeof(ordered_names[0]) - 1] = '\0';
                ordered_count++;
            }
        }
        int idx = -1;
        for (int i = 0; i < ordered_count; i++) {
            if (strcmp(ordered_names[i], range_name) == 0) { idx = i; break; }
        }
        if (idx >= 0) {
            int neighbor_idx = (strcmp(action, "move_range_up") == 0) ? idx - 1 : idx + 1;
            if (neighbor_idx >= 0 && neighbor_idx < ordered_count) {
                swap_named_ranges(range_name, ordered_names[neighbor_idx]);
            }
        }
    }

    cJSON_Delete(eligible_now);

    cJSON *ctx = build_reorder_context();
    if (error) cJSON_AddStringToObject(ctx, "error", error);
    return webserver_render_response("setup/named_ranges_reorder.html", ctx);
}

http_response_t *view_named_range(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        const char *range_name = request_get_query_param(req, "name");
        const char *sort_by = request_get_query_param(req, "sort");
        if (!sort_by) sort_by = "name";

        cJSON *model = lighting_get_settings();
        cJSON *named_ranges = model ? cJSON_GetObjectItem(model, "named_ranges") : NULL;
        cJSON *existing = (range_name && named_ranges) ? cJSON_GetObjectItem(named_ranges, range_name) : NULL;

        if (range_name && existing) {
            strncpy(g_editing_range_name, range_name, sizeof(g_editing_range_name) - 1);
            g_editing_range_name[sizeof(g_editing_range_name) - 1] = '\0';
            g_has_editing_range_name = true;
            set_selected_leds(cJSON_Duplicate(existing, 1));
        } else {
            g_has_editing_range_name = false;
            g_editing_range_name[0] = '\0';
            set_selected_leds(cJSON_CreateArray());
        }

        if (animation_is_running()) animation_pause();

        leds_clear();
        apply_led_identify(selected_leds());

        cJSON *ctx = build_named_range_context(sort_by);
        cJSON_AddStringToObject(ctx, "page_title", "Named Ranges");
        return webserver_render_response("setup/led_picker.html", ctx);
    }

    /* POST: save or delete a named range. */
    const char *action = request_get_form_field(req, "action");
    const char *old_name = request_get_form_field(req, "old_name");
    const char *range_name = request_get_form_field(req, "range_name");
    const char *selected_leds_str = request_get_form_field(req, "selected_leds");
    if (!action) action = "save";

    cJSON *tokens = parse_selected_tokens(selected_leds_str);

    cJSON *model = lighting_get_settings();
    cJSON *named_ranges = NULL;
    if (model) {
        named_ranges = cJSON_GetObjectItem(model, "named_ranges");
        if (!named_ranges) {
            named_ranges = cJSON_CreateObject();
            cJSON_AddItemToObject(model, "named_ranges", named_ranges);
        }
    }

    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

    if (strcmp(action, "delete") == 0 && old_name && old_name[0] && named_ranges &&
        cJSON_GetObjectItem(named_ranges, old_name)) {
        cJSON_DeleteItemFromObject(named_ranges, old_name);
        persistent_dict_mark_dirty(store); persistent_dict_save(store);
    } else if (range_name && range_name[0] && named_ranges) {
        if (has_cycle(named_ranges, range_name, tokens)) {
            cJSON_Delete(tokens);
            return response_create(400, "text/plain", "Circular named-range reference detected");
        }

        cJSON_DeleteItemFromObject(named_ranges, range_name);
        cJSON_AddItemToObject(named_ranges, range_name, cJSON_Duplicate(tokens, 1));

        if (old_name && old_name[0] && strcmp(old_name, range_name) != 0 &&
            cJSON_GetObjectItem(named_ranges, old_name)) {
            cJSON_DeleteItemFromObject(named_ranges, old_name);
            rename_named_range_refs(model, old_name, range_name);
        }
        persistent_dict_mark_dirty(store); persistent_dict_save(store);
    }

    cJSON_Delete(tokens);

    g_has_editing_range_name = false;
    g_editing_range_name[0] = '\0';
    set_selected_leds(cJSON_CreateArray());

    animation_resume();
    leds_clear();
    leds_show();

    cJSON *ctx = build_named_range_context("name");
    cJSON_AddStringToObject(ctx, "page_title", "Named Ranges");
    return webserver_render_response("setup/led_picker.html", ctx);
}

http_response_t *view_named_range_set(http_request_t *req)
{
    const char *selected_leds_str = request_get_form_field(req, "selected_leds");
    cJSON *tokens = parse_selected_tokens(selected_leds_str);
    set_selected_leds(tokens);
    tokens = selected_leds();

    cJSON *direct = cJSON_CreateArray();
    for (cJSON *item = tokens->child; item; item = item->next) {
        if (cJSON_IsNumber(item)) {
            bool dup = false;
            for (cJSON *d = direct->child; d; d = d->next) {
                if (d->valueint == item->valueint) { dup = true; break; }
            }
            if (!dup) cJSON_AddItemToArray(direct, cJSON_CreateNumber(item->valuedouble));
        }
    }

    int indices[MAX_RESOLVED_LEDS];
    int idx_count = resolve_tokens(tokens, indices, MAX_RESOLVED_LEDS);

    if (idx_count > 0) {
        leds_identify(indices, idx_count);
    } else {
        leds_clear();
        leds_show();
    }

    cJSON *resolved = cJSON_CreateArray();
    for (int i = 0; i < idx_count; i++) cJSON_AddItemToArray(resolved, cJSON_CreateNumber(indices[i]));

    cJSON *selected_tokens_str_arr = cJSON_CreateArray();
    for (cJSON *item = tokens->child; item; item = item->next) {
        char buf[80];
        if (cJSON_IsNumber(item)) snprintf(buf, sizeof(buf), "%d", item->valueint);
        else snprintf(buf, sizeof(buf), "%s", item->valuestring ? item->valuestring : "");
        cJSON_AddItemToArray(selected_tokens_str_arr, cJSON_CreateString(buf));
    }

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(payload, "selected_tokens", selected_tokens_str_arr);
    cJSON_AddItemToObject(payload, "direct_selected_leds", direct);
    cJSON_AddItemToObject(payload, "resolved_leds", resolved);

    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    http_response_t *res = response_create(200, "application/json", json ? json : "{}");
    if (json) cJSON_free(json);
    return res;
}

http_response_t *view_named_range_remove_subrange(http_request_t *req)
{
    const char *token = request_get_query_param(req, "token");
    const char *sort_by = request_get_query_param(req, "sort");
    if (!sort_by) sort_by = "name";

    if (token && strncmp(token, "named:", 6) == 0) {
        cJSON *tokens = selected_leds();
        cJSON *filtered = cJSON_CreateArray();
        for (cJSON *item = tokens->child; item; item = item->next) {
            if (cJSON_IsString(item) && item->valuestring && strcmp(item->valuestring, token) == 0) continue;
            cJSON_AddItemToArray(filtered, cJSON_Duplicate(item, 1));
        }
        set_selected_leds(filtered);
    }

    apply_led_identify(selected_leds());

    cJSON *ctx = build_named_range_context(sort_by);
    cJSON_AddStringToObject(ctx, "page_title", "Named Ranges");
    return webserver_render_response("setup/led_picker.html", ctx);
}

void add_named_ranges_summary_context(cJSON *ctx)
{
    cJSON *entries = build_named_range_entries("name");
    /* get_named_range_summary_entries() strips "min_led" before rendering. */
    for (cJSON *entry = entries->child; entry; entry = entry->next) {
        cJSON_DeleteItemFromObject(entry, "min_led");
    }
    cJSON_AddItemToObject(ctx, "named_ranges", entries);
}

http_response_t *view_named_range_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    add_named_ranges_summary_context(ctx);
    return webserver_render_response("setup/named_ranges_summary.html", ctx);
}

