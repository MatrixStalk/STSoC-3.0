# Source HUD skeleton

The HUD can use replaceable hands meshes driven by animations stored in a
Source-rigged item OGF (or in an external OMF). The feature is detected automatically when both
models contain these bones:

- `ValveBiped.Bip01_Spine4`
- `ValveBiped.Bip01_L_Clavicle`
- `ValveBiped.Bip01_R_Clavicle`

Bone matching is name-based. Bone indices may differ, and optional mesh bones
such as `ValveBiped.Bip01_L_Ulna`, `ValveBiped.Bip01_L_Wrist` and their right
hand equivalents do not have to exist in the item skeleton. If a helper exists
in both skeletons it is retargeted like any other common ValveBiped bone; if it
only exists in the hands mesh it keeps its bind transform and follows its
retargeted parent.

## Conversion requirements

1. Keep the ValveBiped bone names from the reference SMD files. X-Ray stores
   them in lower case internally, so name case in an LTX file is not important.
2. The engine accepts several independent roots in one OGF (`Camera`, `body`,
   `mag`, and similar roots). Every root branch is evaluated in model space;
   no destructive reparenting of already converted animation tracks is needed.
3. Common ValveBiped bones may use different bind poses, proportions and numeric
   indices. The runtime converts each source pose to parent-local space, removes
   the source bind pose, transfers the rotational animation delta onto the hands
   bind pose, and keeps the hands bind translation/bone length. `Spine4`, the
   logical HUD arm root, also keeps its authored translation delta.
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
rotation relative to the bind pose is processed as a first-person gameplay
camera animator. It directly changes the actor camera direction used by the
view and firing code, in the same camera-effector stage as camera `.anm` files.
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
; path, speed, power, blend_in_seconds, blend_out_seconds
movement_layer_3 = movement\walk.anm, 1.0, 0.50, 0.25, 0.30
movement_layer_4 = movement\walk.anm, 1.0, 0.60, 0.22, 0.32
movement_layer_6 = movement\idle_aim.anm, 0.5, 0.20, 0.22, 0.32
movement_layer_7 = movement\idle.anm, 0.5, 0.70, 0.22, 0.32
```

Layers 6 and 7 provide the aiming and normal idle foundations. The selected
walk/run layer is composed on top, matching the original procedural movement
setup. Configurations containing only layers 0 through 5 remain valid.

The transition weight uses a minimum-jerk S-curve. The first three values remain
compatible with old configurations; omitted transition times default to 0.4
seconds. Movement matrices are applied around the HUD root, avoiding pivots
whose position and axes depend on a particular hands rig.

For merged Source skeletons, non-idle HUD aliases are treated as stop-at-end
motions when the exported motion definition does not contain that flag. Idle
aliases remain cyclic; `_start` and `_end` idle transitions stop normally. The
default can be overridden per alias, for example:

```ini
anm_inspect_stop_at_end = true
anm_custom_loop_stop_at_end = false
```

## Modern recoil

Player weapons use a two-spring recoil model. The camera spring changes the
actual view/fire direction; the faster HUD spring adds receiver and stock
movement without replacing authored firing motions. Existing recoil values
(`cam_dispersion`, `cam_dispersion_inc`, `cam_max_angle`, and horizontal limits)
remain the base force and safety limits. NPC shooting keeps the legacy model.

All new values are optional:

```ini
cam_recoil_modern            = true
cam_recoil_frequency         = 18.0
cam_recoil_damping           = 0.78
cam_recoil_impulse           = 0.55
cam_recoil_return_delay      = 0.075
cam_recoil_return_speed      = 7.5
cam_recoil_max_pitch         = 8.0
cam_recoil_max_yaw           = 3.0
cam_recoil_vertical_random   = 0.10
cam_recoil_horizontal        = 0.32
cam_recoil_horizontal_random = 0.45
cam_recoil_direction_change  = 0.22
cam_recoil_roll              = 0.08
cam_recoil_first_shot        = 1.12
cam_recoil_burst_growth      = 0.035
cam_recoil_burst_limit       = 1.32
cam_recoil_burst_reset       = 0.20
cam_recoil_zoom_k            = 0.72
cam_recoil_crouch_k          = 0.82

hud_recoil_kick              = 0.018
hud_recoil_up                = 0.0045
hud_recoil_pitch             = 1.15
hud_recoil_yaw               = 0.35
hud_recoil_roll              = 0.45
hud_recoil_frequency         = 23.0
hud_recoil_damping           = 0.66
```

HUD positions use HUD metres; HUD rotation values use degrees. Set
`cam_recoil_modern = false` in a weapon section to restore its legacy recoil.
