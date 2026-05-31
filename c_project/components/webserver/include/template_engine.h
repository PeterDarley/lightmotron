#ifndef TEMPLATE_ENGINE_H
#define TEMPLATE_ENGINE_H

#include "cJSON.h"

/**
 * Render a template string with the given context.
 *
 * Processes: for loops → conditionals → variable substitution → includes.
 * Returns newly allocated string (caller must free).
 */
char *template_render_string(const char *template_str, cJSON *context);

/**
 * Render a template file from the templates directory.
 *
 * Loads the file, renders with context, returns newly allocated string.
 */
char *template_render_file(const char *filename, cJSON *context);

/**
 * Resolve a variable name against the context (supports dot notation).
 *
 * Returns the cJSON node (not a copy — do not free). NULL if not found.
 */
cJSON *template_resolve_var(const char *var_name, cJSON *context);

/**
 * Evaluate a conditional expression against the context.
 *
 * Supports: ==, !=, in, not in, and, or, not, truthiness.
 */
bool template_eval_condition(const char *expr, cJSON *context);

#endif /* TEMPLATE_ENGINE_H */
