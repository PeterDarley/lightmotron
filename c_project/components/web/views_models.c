#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "cJSON.h"
#include "esp_err.h"

#include <string.h>
#include <stdlib.h>

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Mirrors _models_context()/get_model_names(): sorted list of model names.
 * lighting_get_model_names() returns insertion order, so sort here since
 * lighting.c doesn't guarantee it (Python's get_model_names() does
 * sorted(...)). */
static cJSON *build_sorted_model_names(void)
{
    cJSON *raw = lighting_get_model_names();
    int total = cJSON_GetArraySize(raw);

    const char **names = total > 0 ? calloc(total, sizeof(char *)) : NULL;
    int count = 0;
    if (names) {
        for (int i = 0; i < total; i++) {
            cJSON *item = cJSON_GetArrayItem(raw, i);
            if (item && cJSON_IsString(item)) names[count++] = item->valuestring;
        }
        qsort(names, count, sizeof(char *), cmp_str);
    }

    cJSON *sorted = cJSON_CreateArray();
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(sorted, cJSON_CreateString(names[i]));
    free(names);
    cJSON_Delete(raw);
    return sorted;
}

static http_response_t *render_models_page(void)
{
    cJSON *ctx = build_global_context();
    cJSON_AddItemToObject(ctx, "models", build_sorted_model_names());
    cJSON_AddStringToObject(ctx, "current_model", lighting_get_current_model());
    cJSON_AddStringToObject(ctx, "page_title", "Models");
    return webserver_render_response("setup/models.html", ctx);
}

http_response_t *view_models(http_request_t *req)
{
    if (req->method == HTTP_METHOD_GET) {
        return render_models_page();
    }

    /* POST: action=set|create|delete|rename. Mirrors ModelsView.post(). */
    const char *action = request_get_form_field(req, "action");
    const char *name = request_get_form_field(req, "name");
    const char *new_name = request_get_form_field(req, "new_name");
    if (!action) action = "set";

    if (strcmp(action, "set") == 0 && name && name[0]) {
        esp_err_t err = lighting_set_current_model(name);
        if (err != ESP_OK) return response_create(400, "text/plain", esp_err_to_name(err));

        /* Instruct HTMX to reload the setup page so summaries update - mirrors
         * the Response(status=200, headers={"HX-Redirect": "/setup"}). */
        http_response_t *res = response_create(200, "text/plain", "");
        res->headers = cJSON_CreateObject();
        cJSON_AddStringToObject(res->headers, "HX-Redirect", "/setup");
        return res;
    } else if (strcmp(action, "create") == 0 && name && name[0]) {
        esp_err_t err = lighting_create_model(name, false);
        if (err != ESP_OK) return response_create(400, "text/plain", esp_err_to_name(err));
    } else if (strcmp(action, "delete") == 0 && name && name[0]) {
        esp_err_t err = lighting_delete_model(name);
        if (err != ESP_OK) return response_create(400, "text/plain", esp_err_to_name(err));
    } else if (strcmp(action, "rename") == 0 && name && name[0] && new_name && new_name[0]) {
        esp_err_t err = lighting_rename_model(name, new_name);
        if (err != ESP_OK) return response_create(400, "text/plain", esp_err_to_name(err));
    }

    return render_models_page();
}

http_response_t *view_models_summary(http_request_t *req)
{
    cJSON *names = lighting_get_model_names();
    int model_count = cJSON_GetArraySize(names);
    cJSON_Delete(names);

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "current_model", lighting_get_current_model());
    cJSON_AddNumberToObject(ctx, "model_count", model_count);
    return webserver_render_response("setup/models_summary.html", ctx);
}

http_response_t *view_models_set(http_request_t *req)
{
    /* Python's ModelsSetView reads the "model" form field (not "name"). */
    const char *name = request_get_form_field(req, "model");
    if (name && name[0]) {
        esp_err_t err = lighting_set_current_model(name);
        if (err != ESP_OK) return response_create(400, "text/plain", esp_err_to_name(err));
    }

    http_response_t *res = response_create(200, "text/plain", "");
    res->headers = cJSON_CreateObject();
    cJSON_AddStringToObject(res->headers, "HX-Redirect", "/setup");
    return res;
}

/* Wraps a legacy flat lighting_settings.json (no "models" key) into a
 * single model, mirroring Lighting.wrap_current_settings_into_model().
 * lighting.c does not expose a dedicated helper for this (it is a one-time
 * migration op, not part of the normal model CRUD surface, and is not
 * reachable from any current template - /models/wrap has no caller in the
 * HTML), so it is implemented here directly against persistent_dict rather
 * than through lighting.h. */
http_response_t *view_models_wrap(http_request_t *req)
{
    persistent_dict_t *store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    if (!store) return response_create(400, "text/plain", "storage unavailable");

    if (persistent_dict_has_key(store, "models")) {
        return response_create(400, "text/plain",
            "lighting_settings already contains models; cannot wrap.");
    }

    const char *model_name = "Model";
    cJSON *all = persistent_dict_get_all(store);
    cJSON *old = cJSON_Duplicate(all, 1);

    int total = all ? cJSON_GetArraySize(all) : 0;
    char **keys = total > 0 ? calloc(total, sizeof(char *)) : NULL;
    int key_count = 0;
    if (keys && all) {
        for (cJSON *item = all->child; item; item = item->next) {
            keys[key_count++] = strdup(item->string);
        }
    }
    for (int i = 0; i < key_count; i++) {
        persistent_dict_delete_key(store, keys[i]);
        free(keys[i]);
    }
    free(keys);

    cJSON *models = cJSON_CreateObject();
    cJSON_AddItemToObject(models, model_name, old ? old : cJSON_CreateObject());
    persistent_dict_set(store, "models", models);
    persistent_dict_set(store, "current_model", cJSON_CreateString(model_name));
    persistent_dict_save(store);

    return render_models_page();
}
