#ifndef EFFECTS_H
#define EFFECTS_H

#include "lighting.h"
#include "cJSON.h"

/**
 * Resolve an effect definition (from effects dict) into an active job.
 */
void effect_resolve(const cJSON *effect_def, active_job_t *job);

/**
 * Resolve an inline job entry (from scene definition) into an active job.
 */
void effect_resolve_inline(const cJSON *job_def, active_job_t *job);

#endif /* EFFECTS_H */
