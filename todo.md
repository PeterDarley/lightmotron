# Lighting System Todo

## Tasks

1. ~~**Set up lighting __init__ to accept custom colors**~~
   - Modify `Lighting.__init__()` to accept optional custom colors parameter
   - Merge custom colors into the `colors` library
   - Make them available for use in patterns and scenes

2. **Add target color tracking array**
   - Create array to store current target colors (one per LED)
   - Set after pattern processing, before filters are applied
   - Filters will modify displayed colors only, not target colors
   - Enables filter logic to know intended color vs displayed color

3. **Enable named ranges as pattern targets**
   - Modify `get_targets()` to support named range references
   - Allow patterns to use named ranges directly (e.g., `"target": "engine_lights"`)
   - Load named ranges from `PersistentDict` storage
   - Integrate with existing target parsing (int, list, string ranges, "all")
