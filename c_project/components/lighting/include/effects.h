#ifndef EFFECTS_H
#define EFFECTS_H

#include "lighting.h"
#include "cJSON.h"

/**
 * Resolve an effect definition (from effects dict) into an active job.
 * filters_dict is the current model's top-level "filters" dict, needed to
 * resolve the effect's "filters" array -- each entry there is a *name*
 * referencing an entry in filters_dict, not an inline filter definition.
 */
void effect_resolve(const cJSON *effect_def, const cJSON *filters_dict, active_job_t *job);

/**
 * Resolve an inline job entry (from scene definition) into an active job.
 */
void effect_resolve_inline(const cJSON *job_def, const cJSON *filters_dict, active_job_t *job);

#endif /* EFFECTS_H */
