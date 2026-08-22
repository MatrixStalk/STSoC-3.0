# Separate weapon addon visuals

The renderer works with the existing scope, silencer and grenade-launcher
attachment states and with the new `magazine`, `foregrip` and `side_rail`
inventory slots. It replaces the old requirement that every addon mesh must be
embedded into every weapon OGF.

## Custom inventory slots

List compatible addon item sections in the normal weapon section:

```ini
[wpn_example]
magazine_addons = mag_ak_standard, mag_ak_extended
foregrip_addons = grip_vertical, grip_angled
side_rail_addons = laser_module, flashlight_module

; Optional visual-only scale for a Source-sized world weapon model:
world_scaling = 0.025
```

All settings of a concrete addon live in that addon's own section. The weapon
section only lists compatible items:

```ini
[grip_vertical]
addon_slot             = foregrip
attach_visual          = weapons\addons\grip_vertical
attach_bone            = foregrip_anchor
attach_position        = 0, 0, 0
attach_rotation        = 0, 0, 0
replaced_bones         = grip_static
icon_offset            = 12, 18
```

A replaceable magazine can be configured as follows:

```ini
[wpn_example]
magazine_addons = mag_ak_standard, mag_ak_extended

[mag_ak_extended]
addon_slot       = magazine
attach_visual    = weapons\addons\mag_ak_extended
attach_bone      = magazine_anchor
visual_attach_bone = root
attach_space     = bone
attach_position  = 0, 0, 0
attach_rotation  = 0, 0, 0
attach_scale     = 1
replaced_bones   = magazine_static
icon_offset      = 12, 18
```

When world and HUD skeletons use different anchors or embedded-part names, keep
them in the same addon section using `world_attach_bone`, `hud_attach_bone`,
`replaced_world_bones` and `replaced_hud_bones`.

The items use the normal inventory `CanAttach`/`Attach`/`Detach` flow. The
installed section is saved and synchronized as an index into the corresponding
weapon compatibility list. Therefore the order of `*_addons` must remain stable
for existing saves.

At this stage the custom slots provide inventory state, rendering, icon, weight
and cost integration. A magazine addon does not yet change ammunition capacity
or reload logic, and a foregrip does not select a separate animation set.

## Addon item section

```ini
[scope_example]
attach_visual           = weapons\addons\scope_example
attach_bone             = addon_scope_anchor
attach_position         = 0, 0, 0
attach_rotation         = 0, 0, 0
replaced_bones          = stock_rear_sight, old_scope_mesh
icon_offset             = 12, 18

; Optional mode-specific overrides:
world_attach_visual     = weapons\addons\scope_example_world
hud_attach_visual       = weapons\addons\scope_example_hud
world_attach_bone       = addon_scope_anchor_world
hud_attach_bone         = addon_scope_anchor_hud
world_attach_position   = 0, 0, 0
world_attach_rotation   = 0, 0, 0
hud_attach_position     = 0, 0, 0
hud_attach_rotation     = 0, 0, 0
world_attach_scale      = 1
hud_attach_scale        = 1

replaced_world_bones    = world_only_part
replaced_hud_bones      = hud_only_part
```

Positions use model units. Rotations use degrees. A replacement bone is hidden
while the corresponding addon is installed and restored when it is removed.
The attachment anchor remains usable even when its mesh is hidden.

HUD addon rendering inherits the complete HUD item transform, including the
weapon's `hud_scale`/`hud_scaling`. Do not apply the same `0.025` again through
`hud_attach_scale` unless the addon model itself uses a different source scale.
World addons inherit the weapon's `world_scaling` in the same way HUD addons
inherit its HUD scale. Therefore a Source-sized weapon and Source-sized addon
normally use `world_scaling = 0.025` on the weapon and
`world_attach_scale = 1` on the addon. Use `world_attach_scale` only for a
relative size difference between the addon and its weapon. `attach_scale` is
the shared fallback for both modes.

`attach_space = bone` (the default) follows the selected animated weapon bone.
If the addon is skeletal, `visual_attach_bone` names its own anchor bone; the
renderer aligns that bone to `attach_bone` every frame. This compensates for a
non-zero pivot while preserving magazine movement during reload animations.
`world_visual_attach_bone` and `hud_visual_attach_bone` are available when the
two addon models have different skeletons.

Use `attach_space = weapon_bone` when an extracted addon mesh retains the
coordinate space of the complete source weapon. The renderer keeps the mesh in
weapon space and applies only `attach_bone`'s animation delta relative to its
bind pose. This avoids double translation and still follows reload animations.
`model` is accepted as an alias for `weapon_bone`.

Use
`attach_space = weapon` for a mesh exported in the coordinate space of the
complete weapon; in that mode `attach_bone` is ignored and only the weapon root
transform plus `attach_position/rotation` is applied. `world_attach_space` and
`hud_attach_space` can override the mode independently. This mode does not
follow an animated magazine bone and is intended only for stationary parts.

The older per-weapon `<slot>_attach_*`, `<slot>_replaced_bones` and
`<slot>_x/y` keys remain supported as fallbacks, but new addons do not need
them. Addon-section values have priority.
