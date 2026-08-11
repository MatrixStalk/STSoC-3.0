# Source HUD skeleton

The HUD can use replaceable hands meshes driven by animations stored in a
Source-rigged item OGF (or in an external OMF). The feature is detected automatically when both
models contain these bones:

- `ValveBiped.Bip01_Spine4`
- `ValveBiped.Bip01_L_Clavicle`
- `ValveBiped.Bip01_R_Clavicle`

Bone matching is name-based. Bone indices may differ, and optional mesh bones
such as `ValveBiped.Bip01_L_Ulna`, `ValveBiped.Bip01_L_Wrist` and their right
hand equivalents do not have to exist in the item skeleton. These helper bones
keep the hands mesh bind transforms and follow their retargeted parent instead
of consuming potentially synthetic weapon animation tracks.

## Conversion requirements

1. Keep the ValveBiped bone names from the reference SMD files. X-Ray stores
   them in lower case internally, so name case in an LTX file is not important.
2. The engine accepts several independent roots in one OGF (`Camera`, `body`,
   `mag`, and similar roots). Every root branch is evaluated in model space;
   no destructive reparenting of already converted animation tracks is needed.
3. Common ValveBiped bones may use different bind poses and numeric indices.
   The runtime transfers the animation in model space using both inverse bind
   matrices, preserving the hands skeleton positions and bone lengths.
4. Put the complete weapon skeleton and its animations in the item OGF or its
   optional OMF. The replaceable hands OGF only needs the hands mesh and the
   ValveBiped skeleton. It may be a skeletal-rigid OGF without any animations.
5. Export weapon geometry without an embedded arms mesh. The separate hands
   visual is rendered by the actor HUD and receives the item pose at runtime.
6. Make sure every exported motion retains its complete frame range. A named
   one-frame motion is only a static pose and cannot reproduce the Source SMD
   animation.

## LTX example

```ini
[actor_hud_source]
visual          = stsoc\hands\Gorka
; visual_2 may point to another mesh. If omitted, visual is duplicated.
ancor_0         = ValveBiped.Bip01_R_Hand
ancor_1         = ValveBiped.Bip01_L_Hand
position        = 0, 0, 0
orientation     = 0, 0, 0

[wpn_source_hud]
item_visual     = stsoc\ar15\reciever_m4_skeleton
attach_place_idx = 0
skeleton_merge  = true
; Uniform scale for the complete HUD (both hands and attached items).
; hud_scale_16x9 may override it for widescreen mode.
hud_scale       = 1.0
item_position   = 0, 0, 0
item_orientation = 0, 0, 0
anm_idle        = idle
anm_idle_empty  = idle_empty
anm_idle_aim    = idle_2
anm_idle_aim_empty = idle_empty_2
anm_show        = draw_empty
anm_show_empty  = draw_empty
anm_hide        = holster
anm_hide_empty  = holster_empty
anm_reload      = reload
anm_reload_empty = reload_empty
anm_shots       = fire_01
anm_shoot_aim   = fire_sights
anm_shot_l      = fire_empty
```

`skeleton_merge` is optional: it defaults to `true` when both visuals have the
Source rig. Set it to `false` to force the legacy separated-hands animation
path. In merge mode the item is placed in the HUD root coordinate system; the
`item_position` and `item_orientation` values should normally remain zero.

Changing `visual`/`visual_2` in the active actor HUD section replaces the hands
mesh. The weapon animations are not copied into every hands model: the item
skeleton is the animation master, and all common `ValveBiped.Bip01_*`
motions are retargeted to the selected hands mesh by name after animation
evaluation. Embedded OGF animations and external OMF animations use the same
runtime path.

If the merged item skeleton contains a separate `Camera` bone, its animated
rotation relative to the bind pose is applied to the first-person game camera.
The bone translation is always ignored. The HUD camera itself is not rotated,
so the relative motion authored between the weapon and `Camera` is preserved.

## Procedural movement `.anm` layers

Source HUD rigs use `[hud_movement_layers]` during ordinary idle-state movement
as well as during weapon actions. These matrix animations are applied after the
regular skeletal motion, so they do not replace firing, aiming, or reload clips.
The legacy sinusoidal weapon bobbing is disabled while these Source movement
layers are available, preventing the two movement effects from accumulating.

Each layer accepts the following values:

```ini
; path, speed, power, blend_in_seconds, blend_out_seconds, pivot_bone
movement_layer_3 = movement\walk.anm, 1.0, 0.50, 0.25, 0.30, lead_gun
movement_layer_4 = movement\sprint_generic.anm, 0.8, 0.25, 0.22, 0.32, lead_gun
```

The transition weight uses a minimum-jerk S-curve. `lead_gun` remains valid for
legacy rigs and maps to `ValveBiped.Bip01_Spine4` on a Source rig. The first
three values remain compatible with old configurations; omitted transition
times default to 0.4 seconds and an omitted pivot applies the animation around
the HUD root.

For merged Source skeletons, non-idle HUD aliases are treated as stop-at-end
motions when the exported motion definition does not contain that flag. Idle
aliases remain cyclic; `_start` and `_end` idle transitions stop normally. The
default can be overridden per alias, for example:

```ini
anm_inspect_stop_at_end = true
anm_custom_loop_stop_at_end = false
```
