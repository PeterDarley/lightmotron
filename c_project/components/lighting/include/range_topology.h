#ifndef RANGE_TOPOLOGY_H
#define RANGE_TOPOLOGY_H

#include <stdbool.h>
#include "lighting.h"

/**
 * True if `range_name` has a 2D topology defined (model.range_topology
 * holds an entry for it). A range defaults to 1D the moment it's created
 * (a plain LED membership list); this is what "the user set it as 2D"
 * means in storage terms.
 */
bool range_topology_exists(const char *range_name);

/**
 * Resolves a 2D-enabled named range directly into `job`: job->target_indices
 * becomes a flat row-major grid (row * job->grid_width + col -> LED index,
 * or -1 for a gap cell with no real LED), job->target_count = grid_width *
 * grid_height, and job->grid_width/grid_height are set. The grid is built
 * from the range's OWN resolved members (sorted ascending, consumed
 * sequentially into rows per the stored segments/gaps), not any global
 * LED index space -- each 2D range has its own independent topology.
 *
 * Returns false (job->target_count left at 0) if the range doesn't exist,
 * has no topology, or resolves to no members.
 */
bool range_topology_resolve(const char *range_name, active_job_t *job);

#endif /* RANGE_TOPOLOGY_H */
