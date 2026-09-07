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

Missile HUD sections, including grenades, may retain their traditional
`visual` key instead of renaming it to `item_visual`. Set `skeleton_merge = true`
explicitly in such a section to make that Source-rigged model drive the external
HUD hands. Without the explicit option, `visual` keeps its legacy complete-model
behavior.

Grenades and other missiles may release the held object at a normalized point
inside `anim_throw_act`/`anm_throw_act` instead of relying on an OGF motion mark:

```ini
throw_act_release_time = 0.46
throw_point            = 0.0, 0.3, 0.2
snd_anm_throw_act_after = weapons\grenade\release
```

`throw_act_release_time` accepts `0..1`; omit it (or use `-1`) to retain motion
mark behavior with animation-end fallback. Both values can be changed live in
the HUD Editor under **Missile / grenade animation timeline**. Timing in the HUD
section overrides the object section; `throw_point` remains an object-section
setting. If `throw_point_bone` is omitted, the point is evaluated from the active
HUD item's root, so it also works with merged Source skeletons.

`snd_anm_throw_act_after` is separate from the animation-start
`snd_anm_throw_act` cue. It is played once from `Throw()`, after the physical
throw parameters are captured and the ownership-reject event is sent. It
therefore follows either the motion mark or the live release timeline exactly.

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

## World models, inventory previews and skinning

Animated weapon and addon world models use their exported `idle` cycle,
including models without `use_hud_model_as_world`. The cycle is selected on
spawn/creation and re-evaluated after pickup removes physics bone callbacks.
World addon anchors and inventory modification markers use that same evaluated
pose. Export an `idle` cycle that assembles the model correctly; an OGF bind
pose is not a substitute for it.

World fire-mode transition animations temporarily replace that base cycle. When
the transition ends, the engine explicitly restores `idle` before applying the
small selector pose; otherwise the live world model used by a 3D inventory icon
can retain or loop the full transition animation.

Inventory previews consume the prepared pose and current bone visibility without
overwriting the live skeleton or its previous-frame history. Inactive rucksack
items retain their last evaluated idle pose. Replaced meshes are restored in the
preview immediately after an attachment is removed, even while the item is inactive.

Deploy `gamedata/shaders/r3/skin.h` together with the renderer: both bone arrays
now hold 256 entries, covering the renderer's accepted IDs 0 through 254. An old
128-entry shader can corrupt the adjacent previous-pose array. The renderer now
checks the reflected array sizes before uploading and logs a `Skinning palette
mismatch` instead of writing beyond them. Restart the game after updating the
shader; manually deleting its shader cache is not required.

Runtime regression checks after rebuilding:

- Fire repeatedly with a Source weapon and a hand-pose addon, then return to
  idle; neither hidden arm copy nor replaced weapon geometry should reappear.
- Install/remove an attachment in the rucksack and move the weapon to/from a
  slot; the icon must keep its assembled idle pose and show the correct parts.
- Drop and pick up the weapon and a standalone addon; their world and inventory
  representations must remain assembled.

### HUD pose update order

The actor updates before its held items. Its early HUD pose is still needed for
collision and fire points, but it is not the final render pose: item `UpdateCL`
can start another animation or change bone visibility afterwards. At the end of
`CLevel::OnFrame`, after the editor update, `finalize_animation_pose` evaluates
the item, its addon proxies and the merged hands together. This does not rerun
inertia, recoil, bobbing or collision; the addon blend advances only at this final
stage. HUD addon rendering only submits prepared models and transforms. World
and inventory animation selection is unaffected by this ordering change.

Runtime verification still required: with recoil disabled, test idle and repeated
fire/action-to-idle transitions, first without a hand-pose addon and then with
one. Also pause/unpause with an attached addon: its cached HUD visual must remain
visible while updates are paused. Source-order checks alone do not establish that
the reported mesh jitter is gone.

## Knife hit and stab timing

Knife sections can replace legacy motion marks with normalized strike times:

```ini
hit_time  = 0.42
stab_time = 0.57
```

`hit_time` controls the primary attack (`eFire`); `stab_time` controls the
secondary attack (`eFire2`). Values are `0..1` across the active attack-start
animation. Omit a value or set it to `-1` to retain motion marks and the legacy
fallback. HUD-section values override the object section. The live values,
animation progress, reset and copy controls are available under **Knife hit / stab
timeline** in the HUD editor. Each attack can apply damage at most once.
