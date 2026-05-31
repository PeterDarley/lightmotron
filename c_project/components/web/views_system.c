#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "lighting.h"
#include "ota_update.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <esp_system.h>
#include <esp_spiffs.h>

static http_response_t *render(const char *tpl, cJSON *ctx)
{
    char *html = template_render_file(tpl, ctx);
    cJSON_Delete(ctx);
    if (!html) return response_create(500, "text/plain", "Template error");
    http_response_t *res = response_create(200, "text/html", html);
    free(html);
    return res;
}

http_response_t *view_backup(http_request_t *req)
{
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!store) return response_create(500, "text/plain", "Error reading settings");

    cJSON *all = persistent_dict_get_all(store);
    if (!all) return response_create(500, "text/plain", "Error reading data");

    /* Duplicate before releasing any implicit lock — cJSON_Print may traverse the tree */
    cJSON *copy = cJSON_Duplicate(all, 1);
    if (!copy) return response_create(500, "text/plain", "Error duplicating data");

    char *json = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    if (!json) return response_create(500, "text/plain", "Error serializing");

    http_response_t *res = response_create(200, "application/json", json);
    cJSON_free(json);
    return res;
}

http_response_t *view_restore(http_request_t *req)
{
    const char *body = req->body;
    if (!body || strlen(body) == 0) return response_create(400, "text/plain", "No data");

    cJSON *parsed = cJSON_Parse(body);
    if (!parsed) return response_create(400, "text/plain", "Invalid JSON");

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    if (!store) {
        cJSON_Delete(parsed);
        return response_create(500, "text/plain", "Error");
    }

    /* Replace all keys from parsed data */
    cJSON *item = parsed->child;
    while (item) {
        persistent_dict_set(store, item->string, cJSON_Duplicate(item, 1));
        item = item->next;
    }
    cJSON_Delete(parsed);
    persistent_dict_save(store);
    return response_redirect("/setup");
}

http_response_t *view_restore_confirm(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    return render("setup/restore_confirm.html", ctx);
}

http_response_t *view_theme(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/system_settings.json");
    if (store) {
        cJSON *theme = persistent_dict_get_dup(store, "theme");
        if (theme && cJSON_IsString(theme)) cJSON_AddStringToObject(ctx, "theme", theme->valuestring);
        cJSON_Delete(theme);
    }
    return render("setup/theme.html", ctx);
}

http_response_t *view_theme_delete(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    if (name && strlen(name) > 0) {
        char path[128];
        snprintf(path, sizeof(path), "/spiffs/www/themes/%s", name);
        remove(path);
    }
    return response_redirect("/setup/theme");
}

http_response_t *view_theme_upload(http_request_t *req)
{
    const char *name = request_get_form_field(req, "name");
    const char *body = req->body;
    if (!name || !body) return response_create(400, "text/plain", "Missing data");

    char path[128];
    snprintf(path, sizeof(path), "/spiffs/www/themes/%s", name);
    FILE *f = fopen(path, "w");
    if (!f) return response_create(500, "text/plain", "Write error");
    fputs(body, f);
    fclose(f);

    persistent_dict_t *store = persistent_dict_open("/spiffs/data/system_settings.json");
    if (store) {
        cJSON *val = cJSON_CreateString(name);
        persistent_dict_set(store, "theme", val);
        persistent_dict_save(store);
    }
    return response_redirect("/setup/theme");
}

http_response_t *view_hostname(http_request_t *req)
{
    const char *hostname = request_get_form_field(req, "hostname");
    if (hostname && strlen(hostname) > 0) {
        persistent_dict_t *store = persistent_dict_open("/spiffs/data/system_settings.json");
        if (store) {
            cJSON *val = cJSON_CreateString(hostname);
            persistent_dict_set(store, "hostname", val);
            persistent_dict_save(store);
        }
    }

    cJSON *ctx = build_global_context();
    return render("setup/hostname.html", ctx);
}

http_response_t *view_system_settings(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/system_settings.json");

    if (action && strcmp(action, "save") == 0 && store) {
        const char *led_count = request_get_form_field(req, "led_count");
        const char *led_pin = request_get_form_field(req, "led_pin");
        const char *brightness = request_get_form_field(req, "brightness");

        if (led_count) persistent_dict_set(store, "led_count", cJSON_CreateNumber(atoi(led_count)));
        if (led_pin) persistent_dict_set(store, "led_pin", cJSON_CreateNumber(atoi(led_pin)));
        if (brightness) persistent_dict_set(store, "brightness", cJSON_CreateNumber(atoi(brightness)));
        persistent_dict_save(store);
    }

    cJSON *ctx = build_global_context();
    if (store) {
        cJSON *all = persistent_dict_get_all(store);
        if (all) cJSON_AddItemToObject(ctx, "system_settings", cJSON_Duplicate(all, 1));
    }
    return render("setup/system_settings.html", ctx);
}

http_response_t *view_system_settings_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/system_settings.json");
    if (store) {
        cJSON *all = persistent_dict_get_all(store);
        if (all) cJSON_AddItemToObject(ctx, "system_settings", cJSON_Duplicate(all, 1));
    }
    return render("setup/system_settings_summary.html", ctx);
}

http_response_t *view_system_reboot(http_request_t *req)
{
    esp_restart();
    /* Not reached */
    return response_create(200, "text/plain", "Rebooting...");
}

http_response_t *view_system_reboot_confirm(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    return render("setup/system_reboot_confirm.html", ctx);
}

http_response_t *view_updates(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    ota_update_info_t info = {0};

    if (action && strcmp(action, "check") == 0) {
        ota_check_for_update(&info);
    } else if (action && strcmp(action, "install") == 0) {
        ota_check_for_update(&info);
        if (info.update_available) {
            ota_apply_update(info.download_url);
            /* If we get here, the update failed */
        }
    } else {
        ota_check_for_update(&info);
    }

    cJSON *ctx = build_global_context();
    cJSON *status = cJSON_CreateObject();
    cJSON_AddBoolToObject(status, "update_available", info.update_available);
    cJSON_AddStringToObject(status, "current_version", info.current_version);
    cJSON_AddStringToObject(status, "latest_version", info.latest_version);
    cJSON_AddItemToObject(ctx, "ota_status", status);
    return render("setup/updates.html", ctx);
}

http_response_t *view_updates_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    ota_update_info_t info = {0};
    ota_check_for_update(&info);
    cJSON *status = cJSON_CreateObject();
    cJSON_AddBoolToObject(status, "update_available", info.update_available);
    cJSON_AddStringToObject(status, "current_version", info.current_version);
    cJSON_AddStringToObject(status, "latest_version", info.latest_version);
    cJSON_AddItemToObject(ctx, "ota_status", status);
    return render("setup/updates_summary.html", ctx);
}

http_response_t *view_custom_colors(http_request_t *req)
{
    const char *action = request_get_form_field(req, "action");
    persistent_dict_t *store = persistent_dict_open("/spiffs/data/lighting_settings.json");
    cJSON *model = lighting_get_settings();
    if (!model) return response_create(500, "text/plain", "Error");

    cJSON *colors = cJSON_GetObjectItem(model, "custom_colors");
    if (!colors) { colors = cJSON_CreateObject(); cJSON_AddItemToObject(model, "custom_colors", colors); }

    if (action && strcmp(action, "save") == 0) {
        const char *name = request_get_form_field(req, "name");
        const char *color = request_get_form_field(req, "color");
        if (name && color) {
            cJSON_DeleteItemFromObject(colors, name);
            cJSON_AddStringToObject(colors, name, color);
            persistent_dict_save(store);
        }
    } else if (action && strcmp(action, "delete") == 0) {
        const char *name = request_get_form_field(req, "name");
        if (name) {
            cJSON_DeleteItemFromObject(colors, name);
            persistent_dict_save(store);
        }
    }

    cJSON *ctx = build_global_context();
    cJSON_AddItemReferenceToObject(ctx, "custom_colors", colors);
    return render("setup/custom_colors.html", ctx);
}

http_response_t *view_custom_colors_summary(http_request_t *req)
{
    cJSON *ctx = build_global_context();
    cJSON *model = lighting_get_settings();
    if (model) {
        cJSON *cc = cJSON_GetObjectItem(model, "custom_colors");
        if (cc) cJSON_AddItemReferenceToObject(ctx, "custom_colors", cc);
    }
    return render("setup/custom_colors_summary.html", ctx);
}
