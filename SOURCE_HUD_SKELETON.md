# Source HUD skeleton

The HUD can use replaceable hands meshes driven by animations stored in a
Source-rigged item OGF/OMF. The feature is detected automatically when both
models contain these bones:

- `ValveBiped.Bip01_Spine4`
- `ValveBiped.Bip01_L_Clavicle`
- `ValveBiped.Bip01_R_Clavicle`

Bone matching is name-based. Bone indices may differ, and optional mesh bones
such as `ValveBiped.Bip01_L_Ulna`, `ValveBiped.Bip01_L_Wrist` and their right
hand equivalents do not have to exist in the item skeleton.

## Conversion requirements

1. Keep the ValveBiped bone names from the reference SMD files. X-Ray stores
   them in lower case internally, so name case in an LTX file is not important.
2. X-Ray OGF supports one skeleton root. Reparent the independent Source roots
   (`Camera`, `body`, `mag`, and similar roots) to one common dummy root during
   conversion. Do the same for every OGF and its OMF.
3. Common ValveBiped bones in the item and hands OGF must have the same bind
   pose. Their numeric indices do not have to match.
4. Put the complete weapon skeleton and its animations in the item OGF/OMF.
   The replaceable hands OGF only needs the hands mesh, the ValveBiped skeleton,
   and a minimal `idle` motion so it is exported as an animated skeleton.
5. Export weapon geometry without an embedded arms mesh. The separate hands
   visual is rendered by the actor HUD and receives the item pose at runtime.

## LTX example

```ini
[actor_hud_source]
visual          = stsoc\hands\source_gorka
; visual_2 may point to another mesh. If omitted, visual is duplicated.
ancor_0         = ValveBiped.Bip01_R_Hand
ancor_1         = ValveBiped.Bip01_L_Hand
position        = 0, 0, 0
orientation     = 0, 0, 0

[wpn_source_hud]
item_visual     = weapons\source\ar15_hud
attach_place_idx = 0
skeleton_merge  = true
item_position   = 0, 0, 0
item_orientation = 0, 0, 0
anm_idle        = idle
```

`skeleton_merge` is optional: it defaults to `true` when both visuals have the
Source rig. Set it to `false` to force the legacy separated-hands animation
path. In merge mode the item is placed in the HUD root coordinate system; the
`item_position` and `item_orientation` values should normally remain zero.

Changing `visual`/`visual_2` in the active actor HUD section replaces the hands
mesh. The weapon OMF is not loaded into every hands model: the item skeleton is
the animation master, and all common `ValveBiped.Bip01_*` transforms are copied
to the selected hands mesh by name after animation evaluation.
