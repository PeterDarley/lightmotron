/* Backup (download) / restore (upload) of persistent settings.
 *
 * Mirrors BackupView / RestoreView / RestoreConfirmView in web/views.py.
 * The Python side keeps everything in a single flat "storage.json" (with
 * "system_settings" and "lighting_settings" as two of its top-level keys),
 * so a backup there is just a dump of the whole file. This C port instead
 * splits persistent state across two files - /data/system_settings.json
 * and /data/lighting_settings.json - so a backup here combines both
 * into one JSON document shaped like {"system_settings": {...},
 * "lighting_settings": {...}} to stay a drop-in match for Python's backup
 * shape (and for what RestoreView expects to read back).
 *
 * This file previously didn't exist as its own translation unit - backup/
 * restore lived inline in views_system.c. It's split out here per the sync
 * plan; see the audit report for the routes.c/CMakeLists.txt entries this
 * requires (this file intentionally does not touch either).
 */
#include "views.h"
#include "webserver.h"
#include "template_engine.h"
#include "persistent_dict.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

/* Replaces the entire contents of `store` with the (adopted) top-level keys
 * of `new_data`. Mirrors PersistentDict.clear() + update() in RestoreView. */
static void replace_all_keys(persistent_dict_t *store, cJSON *new_data)
{
    cJSON *all = persistent_dict_get_all(store);
    if (all) {
        int total = cJSON_GetArraySize(all);
        char **keys = total > 0 ? calloc(total, sizeof(char *)) : NULL;
        int count = 0;
        if (keys) {
            for (cJSON *item = all->child; item; item = item->next) keys[count++] = strdup(item->string);
        }
        for (int i = 0; i < count; i++) {
            persistent_dict_delete_key(store, keys[i]);
            free(keys[i]);
        }
        free(keys);
    }

    if (new_data) {
        for (cJSON *item = new_data->child; item; item = item->next) {
            persistent_dict_set(store, item->string, cJSON_Duplicate(item, 1));
        }
    }
}

http_response_t *view_backup(http_request_t *req)
{
    persistent_dict_t *sys_store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    persistent_dict_t *lighting_store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);
    if (!sys_store || !lighting_store) return response_create(500, "text/plain", "Error reading settings");

    cJSON *sys_all = persistent_dict_get_all(sys_store);
    cJSON *sys_copy = sys_all ? cJSON_Duplicate(sys_all, 1) : cJSON_CreateObject();

    /* Strip WiFi credentials entirely so restoring a backup on a different
     * device doesn't overwrite its network config - mirrors BackupView. */
    cJSON_DeleteItemFromObject(sys_copy, "wifi");

    cJSON *lighting_all = persistent_dict_get_all(lighting_store);
    cJSON *lighting_copy = lighting_all ? cJSON_Duplicate(lighting_all, 1) : cJSON_CreateObject();

    cJSON *combined = cJSON_CreateObject();
    cJSON_AddItemToObject(combined, "system_settings", sys_copy);
    cJSON_AddItemToObject(combined, "lighting_settings", lighting_copy);

    char *json = cJSON_PrintUnformatted(combined);
    cJSON_Delete(combined);
    if (!json) return response_create(500, "text/plain", "Error serializing");

    http_response_t *res = response_create(200, "application/json", json);
    cJSON_free(json);
    res->headers = cJSON_CreateObject();
    cJSON_AddStringToObject(res->headers, "Content-Disposition",
                            "attachment; filename=\"lightmotron_backup.json\"");
    return res;
}

http_response_t *view_restore(http_request_t *req)
{
    const char *json_text = request_get_form_field(req, "backup_json");
    if (!json_text || !json_text[0]) {
        return response_redirect("/status?restore=empty");
    }

    cJSON *restored = cJSON_Parse(json_text);
    if (!restored || !cJSON_IsObject(restored)) {
        if (restored) cJSON_Delete(restored);
        return response_redirect("/status?restore=invalid");
    }

    cJSON *sys_data = cJSON_GetObjectItem(restored, "system_settings");
    cJSON *lighting_data = cJSON_GetObjectItem(restored, "lighting_settings");

    persistent_dict_t *sys_store = persistent_dict_open(STORAGE_SYSTEM_SETTINGS_FILE);
    persistent_dict_t *lighting_store = persistent_dict_open(STORAGE_LIGHTING_SETTINGS_FILE);

    if (sys_store && sys_data) {
        replace_all_keys(sys_store, sys_data);
        persistent_dict_save(sys_store);
    }
    if (lighting_store && lighting_data) {
        replace_all_keys(lighting_store, lighting_data);
        persistent_dict_save(lighting_store);
    }

    cJSON_Delete(restored);
    return response_redirect("/status?restore=ok");
}

http_response_t *view_restore_confirm(http_request_t *req)
{
    char *backup_json = NULL;

    for (int i = 0; i < req->file_count; i++) {
        if (strcmp(req->files[i].field_name, "backup_file") == 0 && req->files[i].data && req->files[i].data_len > 0) {
            backup_json = malloc(req->files[i].data_len + 1);
            if (backup_json) {
                memcpy(backup_json, req->files[i].data, req->files[i].data_len);
                backup_json[req->files[i].data_len] = '\0';
            }
            break;
        }
    }

    if (!backup_json) {
        const char *form_json = request_get_form_field(req, "backup_json");
        if (form_json && form_json[0]) backup_json = strdup(form_json);
    }

    if (!backup_json || !backup_json[0]) {
        free(backup_json);
        return response_create(200, "text/html",
            "<div class=\"alert alert-warning small py-2 mb-2\">No backup JSON file selected.</div>");
    }

    cJSON *parsed = cJSON_Parse(backup_json);
    if (!parsed) {
        free(backup_json);
        return response_create(200, "text/html",
            "<div class=\"alert alert-danger small py-2 mb-2\">Selected file does not contain valid JSON.</div>");
    }
    if (!cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        free(backup_json);
        return response_create(200, "text/html",
            "<div class=\"alert alert-danger small py-2 mb-2\">Backup JSON must contain a top-level object.</div>");
    }
    cJSON_Delete(parsed);

    cJSON *ctx = build_global_context();
    cJSON_AddStringToObject(ctx, "backup_json", backup_json);
    free(backup_json);
    return webserver_render_response("setup/restore_confirm.html", ctx);
}
