#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

static http_response_t *render(const char *tpl, cJSON *ctx)
{
    char *html = template_render_file(tpl, ctx);
    cJSON_Delete(ctx);
    if (!html) return response_create(500, "text/plain", "Template error");
    http_response_t *res = response_create(200, "text/html", html);
    free(html);
    return res;
}

http_response_t *view_named_range(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();

    if (model) {
        cJSON *nr = cJSON_GetObjectItem(model, "named_ranges");
        if (nr) cJSON_AddItemReferenceToObject(ctx, "named_ranges", nr);
    }

    const char *name = request_get_query_param(req, "name");
    if (name) {
        cJSON_AddStringToObject(ctx, "range_name", name);
        cJSON *nr = cJSON_GetObjectItem(model, "named_ranges");
        if (nr) {
            cJSON *range = cJSON_GetObjectItem(nr, name);
            if (range) cJSON_AddItemReferenceToObject(ctx, "range", range);
        }
    }

    /* LED count for picker */
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/system_settings.json");
    int led_count = 30;
    if (store) {
        cJSON *sys = persistent_dict_get_dup(store, "neopixels");
        if (sys && cJSON_IsArray(sys) && cJSON_GetArraySize(sys) > 0) {
            cJSON *first = cJSON_GetArrayItem(sys, 0);
            cJSON *num = cJSON_GetObjectItem(first, "num");
            if (num && cJSON_IsNumber(num)) led_count = num->valueint;
        }
        cJSON_Delete(sys);
    }
    cJSON_AddNumberToObject(ctx, "led_count", led_count);

    return render("setup/named_ranges_summary.html", ctx);
}

http_response_t *view_named_range_set(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *action = request_get_form_field(req, "action");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model || !name) return response_create(400, "text/plain", "Bad Request");

    cJSON *named_ranges = cJSON_GetObjectItem(model, "named_ranges");
    if (!named_ranges) { named_ranges = cJSON_CreateObject(); cJSON_AddItemToObject(model, "named_ranges", named_ranges); }

    if (action && strcmp(action, "delete") == 0) {
        cJSON_DeleteItemFromObject(named_ranges, name);
        persistent_dict_save(store);
    } else if (action && strcmp(action, "save") == 0) {
        const char *new_name = request_get_form_field(req, "new_name");
        const char *indices_str = request_get_form_field(req, "indices");

        cJSON *indices_arr = cJSON_CreateArray();
        if (indices_str && strlen(indices_str) > 0) {
            char *copy = strdup(indices_str);
            char *token = strtok(copy, ",");
            while (token) {
                while (*token == ' ') token++;
                if (strncmp(token, "named:", 6) == 0) {
                    cJSON_AddItemToArray(indices_arr, cJSON_CreateString(token));
                } else {
                    cJSON_AddItemToArray(indices_arr, cJSON_CreateNumber(atoi(token)));
                }
                token = strtok(NULL, ",");
            }
            free(copy);
        }

        cJSON_DeleteItemFromObject(named_ranges, name);
        const char *final_name = (new_name && strlen(new_name) > 0) ? new_name : name;
        cJSON_AddItemToObject(named_ranges, final_name, indices_arr);
        persistent_dict_save(store);
    }

    cJSON *ctx = build_global_context();
    cJSON_AddItemReferenceToObject(ctx, "named_ranges", named_ranges);
    return render("setup/named_ranges_summary.html", ctx);
}

http_response_t *view_named_range_remove_subrange(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *index_str = request_get_form_field(req, "index");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model || !name || !index_str) return response_create(400, "text/plain", "Bad Request");

    int remove_idx = atoi(index_str);
    cJSON *named_ranges = cJSON_GetObjectItem(model, "named_ranges");
    if (named_ranges) {
        cJSON *range = cJSON_GetObjectItem(named_ranges, name);
        if (range && cJSON_IsArray(range)) {
            if (remove_idx >= 0 && remove_idx < cJSON_GetArraySize(range)) {
                cJSON_DeleteItemFromArray(range, remove_idx);
                persistent_dict_save(store);
            }
        }
    }

    cJSON *ctx = build_global_context();
    if (named_ranges) cJSON_AddItemReferenceToObject(ctx, "named_ranges", named_ranges);
    return render("setup/named_ranges_summary.html", ctx);
}

http_response_t *view_named_range_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *nr = cJSON_GetObjectItem(model, "named_ranges");
        if (nr) cJSON_AddItemReferenceToObject(ctx, "named_ranges", nr);
    }
    return render("setup/named_ranges_summary.html", ctx);
}
