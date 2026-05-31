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

http_response_t *view_models(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model) return response_create(500, "text/plain", "Error");

    cJSON *models = cJSON_GetObjectItem(model, "models");
    if (!models) { models = cJSON_CreateObject(); cJSON_AddItemToObject(model, "models", models); }

    if (action && strcmp(action, "create") == 0) {
        const char *name = request_get_form_field(req, "name");
        if (name && strlen(name) > 0) {
            cJSON *new_model = cJSON_CreateObject();
            cJSON_AddItemToObject(models, name, new_model);
            persistent_dict_save(store);
        }
    } else if (action && strcmp(action, "delete") == 0) {
        const char *name = request_get_form_field(req, "name");
        if (name) {
            cJSON_DeleteItemFromObject(models, name);
            persistent_dict_save(store);
        }
    } else if (action && strcmp(action, "duplicate") == 0) {
        const char *name = request_get_form_field(req, "name");
        const char *new_name = request_get_form_field(req, "new_name");
        if (name && new_name && strlen(new_name) > 0) {
            cJSON *src = cJSON_GetObjectItem(models, name);
            if (src) {
                cJSON *dup = cJSON_Duplicate(src, 1);
                cJSON_AddItemToObject(models, new_name, dup);
                persistent_dict_save(store);
            }
        }
    }

    cJSON *ctx = build_global_context();
    cJSON_AddItemReferenceToObject(ctx, "models", models);
    return render("setup/models.html", ctx);
}

http_response_t *view_models_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *models = cJSON_GetObjectItem(model, "models");
        if (models) cJSON_AddItemReferenceToObject(ctx, "models", models);
    }
    return render("setup/models_summary.html", ctx);
}

http_response_t *view_models_set(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    if (name) {
        lighting_set_current_model(name);
    }
    return response_redirect("/setup/models");
}

http_response_t *view_models_wrap(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/system_settings.json");

    if (action && store) {
        if (strcmp(action, "enable") == 0) {
            persistent_dict_set(store, "wrap_leds", cJSON_CreateBool(true));
        } else if (strcmp(action, "disable") == 0) {
            persistent_dict_set(store, "wrap_leds", cJSON_CreateBool(false));
        }
        persistent_dict_save(store);
    }

    cJSON *ctx = build_global_context();
    return render("setup/models_summary.html", ctx);
}
