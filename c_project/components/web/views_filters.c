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

http_response_t *view_filters(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *f = cJSON_GetObjectItem(model, "filters");
        if (f) cJSON_AddItemReferenceToObject(ctx, "filters", f);
    }
    return render("setup/filters.html", ctx);
}

http_response_t *view_filter_edit(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *action = request_get_form_field(req, "action");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model || !name) return response_create(400, "text/plain", "Bad Request");

    cJSON *filters = cJSON_GetObjectItem(model, "filters");
    if (!filters) { filters = cJSON_CreateObject(); cJSON_AddItemToObject(model, "filters", filters); }

    if (action && strcmp(action, "delete") == 0) {
        cJSON_DeleteItemFromObject(filters, name);
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "filters", filters);
        return render("setup/filters_summary.html", ctx);
    }

    if (action && strcmp(action, "save") == 0) {
        const char *new_name = request_get_form_field(req, "new_name");
        const char *filter_type = request_get_form_field(req, "type");
        const char *amount = request_get_form_field(req, "amount");
        const char *chance = request_get_form_field(req, "chance");

        cJSON *filter = cJSON_GetObjectItem(filters, name);
        if (!filter) { filter = cJSON_CreateObject(); cJSON_AddItemToObject(filters, name, filter); }

        if (filter_type) { cJSON_DeleteItemFromObject(filter, "type"); cJSON_AddStringToObject(filter, "type", filter_type); }
        if (amount) { cJSON_DeleteItemFromObject(filter, "amount"); cJSON_AddNumberToObject(filter, "amount", atof(amount)); }
        if (chance) { cJSON_DeleteItemFromObject(filter, "chance"); cJSON_AddNumberToObject(filter, "chance", atof(chance)); }

        if (new_name && strlen(new_name) > 0 && strcmp(new_name, name) != 0) {
            cJSON *data = cJSON_DetachItemFromObject(filters, name);
            if (data) cJSON_AddItemToObject(filters, new_name, data);
        }
        persistent_dict_save(store);
        cJSON *ctx = build_global_context();
        cJSON_AddItemReferenceToObject(ctx, "filters", filters);
        return render("setup/filters_summary.html", ctx);
    }

    /* Show edit form */
    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "filter_name", name);
    cJSON *filter = cJSON_GetObjectItem(filters, name);
    if (filter) cJSON_AddItemReferenceToObject(ctx, "filter", filter);
    return render("setup/filter_edit.html", ctx);
}

http_response_t *view_filters_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *f = cJSON_GetObjectItem(model, "filters");
        if (f) cJSON_AddItemReferenceToObject(ctx, "filters", f);
    }
    return render("setup/filters_summary.html", ctx);
}
