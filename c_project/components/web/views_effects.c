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

http_response_t *view_effects(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *e = cJSON_GetObjectItem(model, "effects");
        if (e) cJSON_AddItemReferenceToObject(ctx, "effects", e);
        cJSON *f = cJSON_GetObjectItem(model, "filters");
        if (f) cJSON_AddItemReferenceToObject(ctx, "filters", f);
    }
    return render("setup/effects.html", ctx);
}

http_response_t *view_effect_edit(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *action = request_get_form_field(req, "action");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model || !name) return response_create(400, "text/plain", "Bad Request");

    cJSON *effects = cJSON_GetObjectItem(model, "effects");
    if (!effects) { effects = cJSON_CreateObject(); cJSON_AddItemToObject(model, "effects", effects); }

    if (action && strcmp(action, "delete") == 0) {
        cJSON_DeleteItemFromObject(effects, name);
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "effects", effects);
        return render("setup/effects_summary.html", ctx);
    }

    if (action && strcmp(action, "save") == 0) {
        const char *new_name = request_get_form_field(req, "new_name");
        const char *pattern = request_get_form_field(req, "pattern");
        const char *target = request_get_form_field(req, "target");
        const char *period = request_get_form_field(req, "period");
        const char *duty = request_get_form_field(req, "duty");

        cJSON *effect = cJSON_GetObjectItem(effects, name);
        if (!effect) { effect = cJSON_CreateObject(); cJSON_AddItemToObject(effects, name, effect); }

        if (pattern) { cJSON_DeleteItemFromObject(effect, "pattern"); cJSON_AddStringToObject(effect, "pattern", pattern); }
        if (target) { cJSON_DeleteItemFromObject(effect, "target"); cJSON_AddStringToObject(effect, "target", target); }
        if (period) { cJSON_DeleteItemFromObject(effect, "period"); cJSON_AddNumberToObject(effect, "period", atoi(period)); }
        if (duty) { cJSON_DeleteItemFromObject(effect, "duty"); cJSON_AddNumberToObject(effect, "duty", atof(duty)); }

        if (new_name && strlen(new_name) > 0 && strcmp(new_name, name) != 0) {
            cJSON *data = cJSON_DetachItemFromObject(effects, name);
            if (data) cJSON_AddItemToObject(effects, new_name, data);
        }
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "effects", effects);
        return render("setup/effects_summary.html", ctx);
    }

    /* Show edit form */
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "effect_name", name);
    cJSON *effect = cJSON_GetObjectItem(effects, name);
    if (effect) cJSON_AddItemReferenceToObject(ctx, "effect", effect);
    cJSON *filters = cJSON_GetObjectItem(model, "filters");
    if (filters) cJSON_AddItemReferenceToObject(ctx, "filters", filters);
    cJSON *cc = cJSON_GetObjectItem(model, "custom_colors");
    if (cc) cJSON_AddItemReferenceToObject(ctx, "custom_colors", cc);
    return render("setup/effect_edit.html", ctx);
}

http_response_t *view_effects_color_select(http_request_t *req)
{
    return view_scenes_color_select(req);
}

http_response_t *view_effects_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *e = cJSON_GetObjectItem(model, "effects");
        if (e) cJSON_AddItemReferenceToObject(ctx, "effects", e);
    }
    return render("setup/effects_summary.html", ctx);
}
