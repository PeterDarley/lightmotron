#include "template_engine.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

static const char *TAG = "template_engine";

#define TEMPLATES_BASE "/spiffs/templates"
#define INITIAL_BUF_SIZE 4096

/* --- Dynamic string buffer --- */

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} strbuf_t;

static void strbuf_init(strbuf_t *buf)
{
    buf->capacity = INITIAL_BUF_SIZE;
    buf->data = malloc(buf->capacity);
    if (!buf->data) {
        buf->capacity = 0;
        buf->len = 0;
        return;
    }
    buf->data[0] = '\0';
    buf->len = 0;
}

static void strbuf_append(strbuf_t *buf, const char *str, size_t len)
{
    if (!buf->data) return;

    if (len == 0) {
        len = strlen(str);
    }
    if (buf->len + len + 1 > buf->capacity) {
        while (buf->len + len + 1 > buf->capacity) {
            buf->capacity *= 2;
        }
        char *new_data = realloc(buf->data, buf->capacity);
        if (!new_data) return;
        buf->data = new_data;
    }
    memcpy(buf->data + buf->len, str, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void strbuf_free(strbuf_t *buf)
{
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
}

/* --- Variable resolution --- */

cJSON *template_resolve_var(const char *var_name, cJSON *context)
{
    if (!var_name || !context) {
        return NULL;
    }

    /* Skip whitespace */
    while (*var_name == ' ') var_name++;
    char clean[256];
    strncpy(clean, var_name, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';

    /* Trim trailing whitespace */
    size_t len = strlen(clean);
    while (len > 0 && clean[len - 1] == ' ') {
        clean[--len] = '\0';
    }

    /* Check for string literal. Guard len >= 2 (matching Python's
     * `len(expression) >= 2` check) so a 0- or 1-char expression never reads
     * clean[len - 1] out of bounds and never mis-treats a single quote
     * character as an (empty) literal. */
    if (len >= 2 &&
        ((clean[0] == '"' && clean[len - 1] == '"') ||
         (clean[0] == '\'' && clean[len - 1] == '\''))) {
        /* Strip quotes and add as temporary item in context.
         * Using the literal content as key under a prefix avoids collisions
         * and ensures proper lifetime (context owns the cJSON node). */
        clean[len - 1] = '\0';
        const char *literal = clean + 1;

        /* Check if we already added this literal */
        char key[270];
        snprintf(key, sizeof(key), "__lit_%s", literal);
        cJSON *existing = cJSON_GetObjectItem(context, key);
        if (existing) {
            return existing;
        }

        cJSON_AddStringToObject(context, key, literal);
        return cJSON_GetObjectItem(context, key);
    }

    /* Dot-notation traversal */
    char *copy = strdup(clean);
    if (!copy) {
        return NULL;
    }

    cJSON *current = context;
    char *token = strtok(copy, ".");

    while (token && current) {
        if (cJSON_IsArray(current)) {
            /* Try numeric index */
            char *endptr;
            long idx = strtol(token, &endptr, 10);
            if (*endptr == '\0') {
                current = cJSON_GetArrayItem(current, (int)idx);
            } else {
                /* Try resolving token as a context variable for dynamic index */
                cJSON *idx_var = cJSON_GetObjectItem(context, token);
                if (idx_var && cJSON_IsNumber(idx_var)) {
                    current = cJSON_GetArrayItem(current, idx_var->valueint);
                } else {
                    current = NULL;
                }
            }
        } else if (cJSON_IsObject(current)) {
            current = cJSON_GetObjectItem(current, token);
        } else {
            current = NULL;
        }
        token = strtok(NULL, ".");
    }

    free(copy);
    return current;
}

/**
 * Convert a cJSON value to a string representation.
 */
static char *value_to_string(cJSON *val)
{
    if (!val) {
        return strdup("");
    }

    if (cJSON_IsString(val)) {
        return strdup(val->valuestring ? val->valuestring : "");
    } else if (cJSON_IsNumber(val)) {
        char buf[64];
        if (val->valuedouble == (double)val->valueint) {
            snprintf(buf, sizeof(buf), "%d", val->valueint);
        } else {
            snprintf(buf, sizeof(buf), "%g", val->valuedouble);
        }
        return strdup(buf);
    } else if (cJSON_IsBool(val)) {
        return strdup(cJSON_IsTrue(val) ? "True" : "False");
    } else if (cJSON_IsNull(val)) {
        return strdup("");
    } else {
        /* Object or array — serialize */
        char *json_str = cJSON_PrintUnformatted(val);
        return json_str ? json_str : strdup("");
    }
}

/**
 * Check truthiness of a cJSON value (Python semantics).
 */
static bool is_truthy(cJSON *val)
{
    if (!val || cJSON_IsNull(val)) {
        return false;
    }
    if (cJSON_IsBool(val)) {
        return cJSON_IsTrue(val);
    }
    if (cJSON_IsNumber(val)) {
        return val->valuedouble != 0.0;
    }
    if (cJSON_IsString(val)) {
        const char *s = val->valuestring;
        if (!s || *s == '\0') return false;
        /* Python: `str(raw) not in ("", "0", "False", "None")` — exact,
         * case-sensitive match. A lowercase "false"/"none" string value is
         * truthy in Python, so this must not use a case-insensitive compare. */
        if (strcmp(s, "0") == 0 || strcmp(s, "False") == 0 ||
            strcmp(s, "None") == 0) {
            return false;
        }
        return true;
    }
    if (cJSON_IsArray(val)) {
        return cJSON_GetArraySize(val) > 0;
    }
    if (cJSON_IsObject(val)) {
        return val->child != NULL;
    }
    return true;
}

/* --- Condition evaluation --- */

/**
 * Compare two string values.
 */
static bool values_equal(const char *a, const char *b)
{
    if (!a && !b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

/**
 * Check membership: value in collection.
 */
static bool check_membership(const char *value, cJSON *collection, cJSON *context)
{
    if (!collection) {
        /* Try resolving as a string — check substring */
        return false;
    }

    if (cJSON_IsArray(collection)) {
        int size = cJSON_GetArraySize(collection);
        for (int i = 0; i < size; i++) {
            cJSON *item = cJSON_GetArrayItem(collection, i);
            char *item_str = value_to_string(item);
            if (item_str && strcmp(value, item_str) == 0) {
                free(item_str);
                return true;
            }
            free(item_str);
        }
    } else if (cJSON_IsObject(collection)) {
        /* Check if value is a key */
        return cJSON_HasObjectItem(collection, value);
    } else if (cJSON_IsString(collection) && collection->valuestring) {
        /* Substring check */
        return strstr(collection->valuestring, value) != NULL;
    }

    return false;
}

bool template_eval_condition(const char *expr, cJSON *context)
{
    if (!expr || !context) {
        return false;
    }

    /* Skip whitespace */
    while (*expr == ' ') expr++;
    char clean[512];
    strncpy(clean, expr, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    size_t len = strlen(clean);
    while (len > 0 && clean[len - 1] == ' ') {
        clean[--len] = '\0';
    }

    /* Handle 'or' (lowest precedence) */
    char *or_pos = strstr(clean, " or ");
    if (or_pos) {
        char left[256], right[256];
        size_t left_len = or_pos - clean;
        if (left_len > sizeof(left) - 1) left_len = sizeof(left) - 1;
        memcpy(left, clean, left_len);
        left[left_len] = '\0';
        strncpy(right, or_pos + 4, sizeof(right) - 1);
        right[sizeof(right) - 1] = '\0';
        return template_eval_condition(left, context) ||
               template_eval_condition(right, context);
    }

    /* Handle 'and' */
    char *and_pos = strstr(clean, " and ");
    if (and_pos) {
        char left[256], right[256];
        size_t left_len = and_pos - clean;
        if (left_len > sizeof(left) - 1) left_len = sizeof(left) - 1;
        memcpy(left, clean, left_len);
        left[left_len] = '\0';
        strncpy(right, and_pos + 5, sizeof(right) - 1);
        right[sizeof(right) - 1] = '\0';
        return template_eval_condition(left, context) &&
               template_eval_condition(right, context);
    }

    /* Handle 'not' prefix (but not ' not in ' — that's handled below, matching
     * Python's `expression.startswith("not ") and " in " not in expression`). */
    if (strncmp(clean, "not ", 4) == 0 && !strstr(clean, " in ")) {
        return !template_eval_condition(clean + 4, context);
    }

    /* Handle 'not in' */
    char *not_in_pos = strstr(clean, " not in ");
    if (not_in_pos) {
        char left_name[256], right_name[256];
        size_t left_len = not_in_pos - clean;
        if (left_len > sizeof(left_name) - 1) left_len = sizeof(left_name) - 1;
        memcpy(left_name, clean, left_len);
        left_name[left_len] = '\0';
        strncpy(right_name, not_in_pos + 8, sizeof(right_name) - 1);
        right_name[sizeof(right_name) - 1] = '\0';

        cJSON *left_val = template_resolve_var(left_name, context);
        cJSON *right_val = template_resolve_var(right_name, context);
        char *left_str = value_to_string(left_val);
        bool result = !check_membership(left_str, right_val, context);
        free(left_str);
        return result;
    }

    /* Handle 'in' */
    char *in_pos = strstr(clean, " in ");
    if (in_pos) {
        char left_name[256], right_name[256];
        size_t left_len = in_pos - clean;
        if (left_len > sizeof(left_name) - 1) left_len = sizeof(left_name) - 1;
        memcpy(left_name, clean, left_len);
        left_name[left_len] = '\0';
        strncpy(right_name, in_pos + 4, sizeof(right_name) - 1);
        right_name[sizeof(right_name) - 1] = '\0';

        cJSON *left_val = template_resolve_var(left_name, context);
        cJSON *right_val = template_resolve_var(right_name, context);
        char *left_str = value_to_string(left_val);
        bool result = check_membership(left_str, right_val, context);
        free(left_str);
        return result;
    }

    /* Handle '!=' (checked before '==', matching Python's explicit ordering —
     * "must check before == to avoid false split on !==" — and matched as a
     * bare 2-char token rather than " != " with mandatory spaces: Python does
     * `"!=" in expression` / `expression.split("!=", 1)` with no space
     * requirement, so `a!=b` must work same as `a != b`. Surrounding
     * whitespace on each side is stripped by template_resolve_var itself. */
    char *neq_pos = strstr(clean, "!=");
    if (neq_pos) {
        char left_name[256], right_name[256];
        size_t left_len = neq_pos - clean;
        if (left_len > sizeof(left_name) - 1) left_len = sizeof(left_name) - 1;
        memcpy(left_name, clean, left_len);
        left_name[left_len] = '\0';
        strncpy(right_name, neq_pos + 2, sizeof(right_name) - 1);
        right_name[sizeof(right_name) - 1] = '\0';

        cJSON *left_val = template_resolve_var(left_name, context);
        cJSON *right_val = template_resolve_var(right_name, context);
        char *left_str = value_to_string(left_val);
        char *right_str = value_to_string(right_val);
        bool result = !values_equal(left_str, right_str);
        free(left_str);
        free(right_str);
        return result;
    }

    /* Handle '==' — likewise a bare 2-char token, no surrounding spaces required. */
    char *eq_pos = strstr(clean, "==");
    if (eq_pos) {
        char left_name[256], right_name[256];
        size_t left_len = eq_pos - clean;
        if (left_len > sizeof(left_name) - 1) left_len = sizeof(left_name) - 1;
        memcpy(left_name, clean, left_len);
        left_name[left_len] = '\0';
        strncpy(right_name, eq_pos + 2, sizeof(right_name) - 1);
        right_name[sizeof(right_name) - 1] = '\0';

        cJSON *left_val = template_resolve_var(left_name, context);
        cJSON *right_val = template_resolve_var(right_name, context);
        char *left_str = value_to_string(left_val);
        char *right_str = value_to_string(right_val);
        bool result = values_equal(left_str, right_str);
        free(left_str);
        free(right_str);
        return result;
    }

    /* Simple truthiness */
    cJSON *val = template_resolve_var(clean, context);
    return is_truthy(val);
}

/* --- Template processing --- */

/**
 * Find matching end tag, respecting nesting.
 */
static const char *find_end_tag(const char *start, const char *open_tag,
                                const char *close_tag)
{
    int depth = 1;
    const char *ptr = start;
    size_t open_len = strlen(open_tag);
    size_t close_len = strlen(close_tag);

    while (*ptr) {
        if (strncmp(ptr, open_tag, open_len) == 0) {
            depth++;
        } else if (strncmp(ptr, close_tag, close_len) == 0) {
            depth--;
            if (depth == 0) {
                return ptr;
            }
        }
        ptr++;
    }
    return NULL;
}

/* Forward declarations so for-loop iterations can run the full per-item
 * pipeline (conditionals/includes/variables) before the outer passes see
 * the expanded text — mirrors lib/webserver.py's _process_for_loops, which
 * calls _process_if_blocks / _process_includes / _apply_context on each
 * loop_context before splicing the rendered chunk back in. Without this,
 * loop variables would never be resolved because the top-level passes only
 * have access to the original (non-looped) context. */
static char *process_conditionals(const char *template_str, cJSON *context);
static char *process_variables(const char *template_str, cJSON *context);
static char *process_includes(const char *template_str, cJSON *context);
static char *process_for_loops(const char *template_str, cJSON *context);

/**
 * Fully render one loop iteration's body against its per-iteration context.
 *
 * Order matches Python's per-iteration pipeline: nested for-loops, then
 * if-blocks, then includes, then variable substitution.
 */
static char *render_loop_iteration(const char *loop_body, cJSON *loop_ctx)
{
    char *step1 = process_for_loops(loop_body, loop_ctx);
    if (!step1) {
        return strdup("");
    }

    char *step2 = process_conditionals(step1, loop_ctx);
    free(step1);
    if (!step2) {
        return strdup("");
    }

    char *step3 = process_includes(step2, loop_ctx);
    free(step2);
    if (!step3) {
        return strdup("");
    }

    char *step4 = process_variables(step3, loop_ctx);
    free(step3);
    return step4 ? step4 : strdup("");
}

/**
 * Process for loops in template.
 */
static char *process_for_loops(const char *template_str, cJSON *context)
{
    strbuf_t output;
    strbuf_init(&output);

    const char *ptr = template_str;

    while (*ptr) {
        const char *tag_start = strstr(ptr, "{%");
        if (!tag_start) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Output text before tag */
        if (tag_start > ptr) {
            strbuf_append(&output, ptr, tag_start - ptr);
        }

        const char *tag_end = strstr(tag_start, "%}");
        if (!tag_end) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Extract tag content */
        size_t tag_content_len = tag_end - tag_start - 2;
        char tag_content[512];
        if (tag_content_len >= sizeof(tag_content)) {
            tag_content_len = sizeof(tag_content) - 1;
        }
        memcpy(tag_content, tag_start + 2, tag_content_len);
        tag_content[tag_content_len] = '\0';

        /* Trim */
        char *tc = tag_content;
        while (*tc == ' ') tc++;
        size_t tc_len = strlen(tc);
        while (tc_len > 0 && tc[tc_len - 1] == ' ') tc[--tc_len] = '\0';

        if (strncmp(tc, "for ", 4) == 0) {
            /* Parse for loop: "for VAR in ITERABLE" or "for K, V in DICT.method()" */
            const char *body_start = tag_end + 2;

            /* Find matching endfor (nesting-aware). If unmatched (malformed
             * template), drop just the opening tag and keep scanning, same
             * as lib/webserver.py's _process_for_loops does on endfor_start == -1. */
            const char *endfor = find_end_tag(body_start, "{% for ", "{% endfor %}");
            if (!endfor) {
                ptr = tag_end + 2;
                continue;
            }

            /* Extract loop body */
            size_t body_len = endfor - body_start;
            char *loop_body = malloc(body_len + 1);
            memcpy(loop_body, body_start, body_len);
            loop_body[body_len] = '\0';

            /* Parse: for VAR in EXPR */
            char var1[64] = "", var2[64] = "";
            char iterable_expr[256] = "";
            bool tuple_unpack = false;

            char *in_pos = strstr(tc + 4, " in ");
            if (in_pos) {
                /* Variables part */
                size_t vars_len = in_pos - (tc + 4);
                char vars[128];
                memcpy(vars, tc + 4, vars_len);
                vars[vars_len] = '\0';

                /* Check for tuple unpacking (k, v) */
                char *comma = strchr(vars, ',');
                if (comma) {
                    tuple_unpack = true;
                    *comma = '\0';
                    strncpy(var1, vars, sizeof(var1) - 1);
                    /* Trim */
                    char *v1 = var1;
                    while (*v1 == ' ') v1++;
                    memmove(var1, v1, strlen(v1) + 1);

                    char *v2_start = comma + 1;
                    while (*v2_start == ' ') v2_start++;
                    strncpy(var2, v2_start, sizeof(var2) - 1);
                    /* Trim trailing */
                    size_t v2len = strlen(var2);
                    while (v2len > 0 && var2[v2len - 1] == ' ') var2[--v2len] = '\0';
                } else {
                    strncpy(var1, vars, sizeof(var1) - 1);
                    /* Trim */
                    char *v1 = var1;
                    while (*v1 == ' ') v1++;
                    memmove(var1, v1, strlen(v1) + 1);
                    size_t v1len = strlen(var1);
                    while (v1len > 0 && var1[v1len - 1] == ' ') var1[--v1len] = '\0';
                }

                /* Iterable expression */
                strncpy(iterable_expr, in_pos + 4, sizeof(iterable_expr) - 1);
                /* Trim */
                char *ie = iterable_expr;
                while (*ie == ' ') ie++;
                memmove(iterable_expr, ie, strlen(ie) + 1);
                size_t ie_len = strlen(iterable_expr);
                while (ie_len > 0 && iterable_expr[ie_len - 1] == ' ') {
                    iterable_expr[--ie_len] = '\0';
                }
            }

            /* Resolve iterable */
            cJSON *iterable = NULL;
            bool items_mode = false;
            bool keys_mode = false;
            bool values_mode = false;
            bool range_mode = false;
            int range_count = 0;

            /* Check for range(n) */
            if (strncmp(iterable_expr, "range(", 6) == 0) {
                range_mode = true;
                char *num_start = iterable_expr + 6;
                char *paren_end = strchr(num_start, ')');
                if (paren_end) {
                    *paren_end = '\0';
                    /* Try as number */
                    char *endptr;
                    range_count = strtol(num_start, &endptr, 10);
                    if (*endptr != '\0') {
                        /* Try as variable */
                        cJSON *range_var = template_resolve_var(num_start, context);
                        if (range_var && cJSON_IsNumber(range_var)) {
                            range_count = range_var->valueint;
                        }
                    }
                }
            }
            /* Check for .items(), .keys(), .values() */
            else if (strstr(iterable_expr, ".items()")) {
                items_mode = true;
                char *dot = strstr(iterable_expr, ".items()");
                *dot = '\0';
                iterable = template_resolve_var(iterable_expr, context);
            } else if (strstr(iterable_expr, ".keys()")) {
                keys_mode = true;
                char *dot = strstr(iterable_expr, ".keys()");
                *dot = '\0';
                iterable = template_resolve_var(iterable_expr, context);
            } else if (strstr(iterable_expr, ".values()")) {
                values_mode = true;
                char *dot = strstr(iterable_expr, ".values()");
                *dot = '\0';
                iterable = template_resolve_var(iterable_expr, context);
            } else {
                iterable = template_resolve_var(iterable_expr, context);
            }

            /* Execute loop. Each iteration must resolve conditionals/includes/
             * variables against its OWN loop_ctx right away (render_loop_iteration)
             * — the top-level passes only ever see the original context, so a
             * loop variable left unresolved here would stay unresolved forever. */
            if (range_mode) {
                for (int i = 0; i < range_count; i++) {
                    cJSON *loop_ctx = cJSON_Duplicate(context, 1);
                    cJSON_AddNumberToObject(loop_ctx, var1, i);
                    char *rendered = render_loop_iteration(loop_body, loop_ctx);
                    if (rendered) {
                        strbuf_append(&output, rendered, 0);
                        free(rendered);
                    }
                    cJSON_Delete(loop_ctx);
                }
            } else if (items_mode && iterable && cJSON_IsObject(iterable)) {
                cJSON *child = iterable->child;
                while (child) {
                    cJSON *loop_ctx = cJSON_Duplicate(context, 1);
                    cJSON_AddStringToObject(loop_ctx, var1, child->string);
                    cJSON_AddItemToObject(loop_ctx, var2, cJSON_Duplicate(child, 1));
                    char *rendered = render_loop_iteration(loop_body, loop_ctx);
                    if (rendered) {
                        strbuf_append(&output, rendered, 0);
                        free(rendered);
                    }
                    cJSON_Delete(loop_ctx);
                    child = child->next;
                }
            } else if (keys_mode && iterable && cJSON_IsObject(iterable)) {
                cJSON *child = iterable->child;
                while (child) {
                    cJSON *loop_ctx = cJSON_Duplicate(context, 1);
                    cJSON_AddStringToObject(loop_ctx, var1, child->string);
                    char *rendered = render_loop_iteration(loop_body, loop_ctx);
                    if (rendered) {
                        strbuf_append(&output, rendered, 0);
                        free(rendered);
                    }
                    cJSON_Delete(loop_ctx);
                    child = child->next;
                }
            } else if (values_mode && iterable && cJSON_IsObject(iterable)) {
                cJSON *child = iterable->child;
                while (child) {
                    cJSON *loop_ctx = cJSON_Duplicate(context, 1);
                    cJSON_AddItemToObject(loop_ctx, var1, cJSON_Duplicate(child, 1));
                    char *rendered = render_loop_iteration(loop_body, loop_ctx);
                    if (rendered) {
                        strbuf_append(&output, rendered, 0);
                        free(rendered);
                    }
                    cJSON_Delete(loop_ctx);
                    child = child->next;
                }
            } else if (iterable && cJSON_IsArray(iterable) && !items_mode &&
                       !keys_mode && !values_mode) {
                /* Plain list iteration. Excluded from items_mode/keys_mode/
                 * values_mode: Python's _resolve_iterable only returns a value
                 * for those suffixes when the base resolves to an actual dict;
                 * otherwise it returns None and the loop is skipped entirely. */
                int size = cJSON_GetArraySize(iterable);
                for (int i = 0; i < size; i++) {
                    cJSON *item = cJSON_GetArrayItem(iterable, i);
                    cJSON *loop_ctx = cJSON_Duplicate(context, 1);

                    if (tuple_unpack && cJSON_IsArray(item) && cJSON_GetArraySize(item) >= 2) {
                        cJSON_AddItemToObject(loop_ctx, var1,
                                              cJSON_Duplicate(cJSON_GetArrayItem(item, 0), 1));
                        cJSON_AddItemToObject(loop_ctx, var2,
                                              cJSON_Duplicate(cJSON_GetArrayItem(item, 1), 1));
                    } else {
                        cJSON_AddItemToObject(loop_ctx, var1, cJSON_Duplicate(item, 1));
                    }

                    char *rendered = render_loop_iteration(loop_body, loop_ctx);
                    if (rendered) {
                        strbuf_append(&output, rendered, 0);
                        free(rendered);
                    }
                    cJSON_Delete(loop_ctx);
                }
            } else if (iterable && !items_mode && !keys_mode && !values_mode) {
                /* Iterable resolved to something that isn't a JSON array (e.g. a
                 * plain object/dict, string, number, or bool). Python's
                 * _resolve_iterable returns the raw value in this case and
                 * _process_for_loops then does `if not isinstance(items, (list,
                 * tuple)): items = [items]` — i.e. the loop runs exactly ONCE
                 * with the loop variable bound to the whole value (NOT a
                 * key-by-key iteration of a dict). Match that quirk exactly. */
                cJSON *loop_ctx = cJSON_Duplicate(context, 1);
                cJSON_AddItemToObject(loop_ctx, var1, cJSON_Duplicate(iterable, 1));
                if (tuple_unpack) {
                    cJSON_AddStringToObject(loop_ctx, var2, "");
                }
                char *rendered = render_loop_iteration(loop_body, loop_ctx);
                if (rendered) {
                    strbuf_append(&output, rendered, 0);
                    free(rendered);
                }
                cJSON_Delete(loop_ctx);
            }
            /* If iterable is NULL (unresolved variable), Python's `if items is
             * not None` gate skips the loop entirely — same as falling through
             * here with no branch taken. */

            free(loop_body);
            ptr = endfor + strlen("{% endfor %}");
        } else {
            /* Not a for tag, output as-is */
            strbuf_append(&output, tag_start, tag_end + 2 - tag_start);
            ptr = tag_end + 2;
        }
    }

    char *result = output.data;
    return result;
}

/**
 * Process if/else/endif blocks.
 */
static char *process_conditionals(const char *template_str, cJSON *context)
{
    strbuf_t output;
    strbuf_init(&output);

    const char *ptr = template_str;

    while (*ptr) {
        const char *tag_start = strstr(ptr, "{%");
        if (!tag_start) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Output text before tag */
        if (tag_start > ptr) {
            strbuf_append(&output, ptr, tag_start - ptr);
        }

        const char *tag_end = strstr(tag_start, "%}");
        if (!tag_end) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Extract tag content */
        size_t tag_content_len = tag_end - tag_start - 2;
        char tag_content[512];
        if (tag_content_len >= sizeof(tag_content)) {
            tag_content_len = sizeof(tag_content) - 1;
        }
        memcpy(tag_content, tag_start + 2, tag_content_len);
        tag_content[tag_content_len] = '\0';

        /* Trim */
        char *tc = tag_content;
        while (*tc == ' ') tc++;
        size_t tc_len = strlen(tc);
        while (tc_len > 0 && tc[tc_len - 1] == ' ') tc[--tc_len] = '\0';

        if (strncmp(tc, "if ", 3) == 0) {
            /* Find matching endif */
            const char *after_if = tag_end + 2;
            const char *endif_pos = find_end_tag(after_if, "{% if ", "{% endif %}");
            if (!endif_pos) {
                ptr = tag_end + 2;
                continue;
            }

            /* Find optional else */
            const char *else_pos = NULL;
            const char *search = after_if;
            int depth = 0;
            while (search < endif_pos) {
                if (strncmp(search, "{% if ", 6) == 0) {
                    depth++;
                } else if (strncmp(search, "{% endif %}", 11) == 0) {
                    depth--;
                } else if (depth == 0 && strncmp(search, "{% else %}", 10) == 0) {
                    else_pos = search;
                    break;
                }
                search++;
            }

            /* Evaluate condition */
            const char *condition = tc + 3;
            bool result = template_eval_condition(condition, context);

            if (result) {
                /* Render true branch */
                const char *true_end = else_pos ? else_pos : endif_pos;
                size_t true_len = true_end - after_if;
                char *true_block = malloc(true_len + 1);
                memcpy(true_block, after_if, true_len);
                true_block[true_len] = '\0';

                char *rendered = process_conditionals(true_block, context);
                if (rendered) {
                    strbuf_append(&output, rendered, 0);
                    free(rendered);
                }
                free(true_block);
            } else if (else_pos) {
                /* Render else branch */
                const char *else_start = else_pos + 10; /* skip "{% else %}" */
                size_t else_len = endif_pos - else_start;
                char *else_block = malloc(else_len + 1);
                memcpy(else_block, else_start, else_len);
                else_block[else_len] = '\0';

                char *rendered = process_conditionals(else_block, context);
                if (rendered) {
                    strbuf_append(&output, rendered, 0);
                    free(rendered);
                }
                free(else_block);
            }

            ptr = endif_pos + strlen("{% endif %}");
        } else {
            /* Not an if tag, output as-is */
            strbuf_append(&output, tag_start, tag_end + 2 - tag_start);
            ptr = tag_end + 2;
        }
    }

    return output.data;
}

/**
 * Process variable substitution ({{ var }}).
 */
static char *process_variables(const char *template_str, cJSON *context)
{
    strbuf_t output;
    strbuf_init(&output);

    const char *ptr = template_str;

    while (*ptr) {
        const char *var_start = strstr(ptr, "{{");
        if (!var_start) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Output text before variable */
        if (var_start > ptr) {
            strbuf_append(&output, ptr, var_start - ptr);
        }

        const char *var_end = strstr(var_start, "}}");
        if (!var_end) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Extract variable name */
        size_t name_len = var_end - var_start - 2;
        char var_name[256];
        if (name_len >= sizeof(var_name)) {
            name_len = sizeof(var_name) - 1;
        }
        memcpy(var_name, var_start + 2, name_len);
        var_name[name_len] = '\0';

        /* Resolve and substitute */
        cJSON *val = template_resolve_var(var_name, context);
        char *str_val = value_to_string(val);
        if (str_val) {
            strbuf_append(&output, str_val, 0);
            free(str_val);
        }

        ptr = var_end + 2;
    }

    return output.data;
}

/**
 * Process include directives.
 */
static char *process_includes(const char *template_str, cJSON *context)
{
    strbuf_t output;
    strbuf_init(&output);

    const char *ptr = template_str;

    while (*ptr) {
        const char *tag_start = strstr(ptr, "{% include '");
        if (!tag_start) {
            /* Also try double quotes */
            tag_start = strstr(ptr, "{% include \"");
        }
        if (!tag_start) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Output text before tag */
        if (tag_start > ptr) {
            strbuf_append(&output, ptr, tag_start - ptr);
        }

        /* Find closing */
        const char *tag_end = strstr(tag_start, "%}");
        if (!tag_end) {
            strbuf_append(&output, ptr, strlen(ptr));
            break;
        }

        /* Extract filename */
        const char *quote_start = strchr(tag_start + 11, '\'');
        if (!quote_start) {
            quote_start = strchr(tag_start + 11, '"');
        }
        if (!quote_start) {
            ptr = tag_end + 2;
            continue;
        }
        quote_start++;

        const char *quote_end = strchr(quote_start, '\'');
        if (!quote_end) {
            quote_end = strchr(quote_start, '"');
        }
        if (!quote_end) {
            ptr = tag_end + 2;
            continue;
        }

        char include_file[256];
        size_t fn_len = quote_end - quote_start;
        if (fn_len < sizeof(include_file)) {
            memcpy(include_file, quote_start, fn_len);
            include_file[fn_len] = '\0';

            /* Render included template */
            char *included = template_render_file(include_file, context);
            if (included) {
                strbuf_append(&output, included, 0);
                free(included);
            }
        }

        ptr = tag_end + 2;
    }

    return output.data;
}

/* --- Public API --- */

char *template_render_string(const char *template_str, cJSON *context)
{
    if (!template_str) {
        return strdup("");
    }

    /* Processing order: for loops → conditionals → variables → includes */
    char *pass1 = process_for_loops(template_str, context);
    if (!pass1) return strdup("");

    char *pass2 = process_conditionals(pass1, context);
    free(pass1);
    if (!pass2) return strdup("");

    char *pass3 = process_variables(pass2, context);
    free(pass2);
    if (!pass3) return strdup("");

    char *pass4 = process_includes(pass3, context);
    free(pass3);
    if (!pass4) return strdup("");

    return pass4;
}

char *template_render_file(const char *filename, cJSON *context)
{
    if (!filename) {
        return strdup("");
    }

    /* Build full path */
    char filepath[384];
    snprintf(filepath, sizeof(filepath), "%s/%s", TEMPLATES_BASE, filename);

    /* Read file */
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGW(TAG, "Template not found: %s", filepath);
        return strdup("");
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return strdup("");
    }

    char *content = malloc(file_size + 1);
    if (!content) {
        fclose(f);
        return strdup("");
    }

    size_t read = fread(content, 1, file_size, f);
    fclose(f);
    content[read] = '\0';

    /* Render */
    char *result = template_render_string(content, context);
    free(content);
    return result;
}
