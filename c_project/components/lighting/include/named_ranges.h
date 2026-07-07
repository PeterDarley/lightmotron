#ifndef NAMED_RANGES_H
#define NAMED_RANGES_H

#include <stdint.h>

/**
 * Resolve a named range to LED indices.
 *
 * Handles recursive "named:X" references and de-duplicates indices, mirroring
 * Lighting.get_targets(). Returns number of indices written.
 */
int named_ranges_resolve(const char *range_name, int *indices, int max_indices);

/**
 * Resolve a target specification string to LED indices.
 *
 * Supports:
 *   - "0-5" (range)
 *   - "1,3,5" (explicit indices)
 *   - "named:RangeName" (named range reference)
 *   - "all" (every configured LED)
 *   - "0-5, named:Front, 10-15" (combinations)
 *
 * Indices are de-duplicated across the whole spec (an index reachable via
 * more than one token is only written once), mirroring
 * Lighting.get_targets(). Returns number of indices written.
 */
int target_spec_resolve(const char *spec, int *indices, int max_indices);

#endif /* NAMED_RANGES_H */
