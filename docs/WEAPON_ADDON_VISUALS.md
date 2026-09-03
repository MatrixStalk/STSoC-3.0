# Separate weapon addon visuals

The renderer works with the existing scope, silencer and grenade-launcher
attachment states and with the `magazine`, `foregrip`, `side_rail` and `handguard`
inventory slots. It replaces the old requirement that every addon mesh must be
embedded into every weapon OGF.

## Custom inventory slots

List compatible addon item sections in the normal weapon section:

```ini
[wpn_example]
magazine_addons = mag_ak_standard, mag_ak_extended
foregrip_addons = grip_vertical, grip_angled
side_rail_addons = laser_module, flashlight_module
handguard_addons = handguard_example

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

; Gameplay parameters of this magazine:
magazine_capacity       = 45
use_reload_animations   = 10
use_magcheck_animations = 10
```

When world and HUD skeletons use different anchors or embedded-part names, keep
them in the same addon section using `world_attach_bone`, `hud_attach_bone`,
`replaced_world_bones` and `replaced_hud_bones`.

The items use the normal inventory `CanAttach`/`Attach`/`Detach` flow. The
installed section is saved and synchronized as an index into the corresponding
weapon compatibility list. Therefore the order of `*_addons` must remain stable
for existing saves.

Custom slots provide inventory state, rendering, icon, weight and cost
integration. Magazine addons also control ammunition capacity and select reload
and magazine-check animation sets.

## Nested addons

An installed addon may advertise compatible children with the same slot-list
keys used by a weapon. The child points back to the parent by section name or
by the parent's slot name:

```ini
[handguard_example]
addon_slot       = handguard
attach_visual    = weapons\addons\handguard_example
attach_bone      = mod_handguard

; Children which become attachable while this addon is installed.
foregrip_addons = grip_vertical

[grip_vertical]
addon_slot       = foregrip
attach_parent    = handguard_example ; `handguard` is also accepted
attach_visual    = weapons\addons\grip_vertical
attach_space     = weapon_bone
attach_bone      = grip_rail           ; bone in the parent addon model
attach_position  = 0, 0, 0
attach_rotation  = 0, 0, 0
```

`hud_attach_parent` and `world_attach_parent` can override `attach_parent` when
the two render models require different chains. A child transform is evaluated
in its parent's visual space, so animation and scaling are inherited through
arbitrarily deep chains. With a parent, `attach_space = weapon` means the
parent visual's origin and `weapon_bone` means a bone in the parent visual.

The compatibility graph is collected recursively at weapon load. The original
three serialized slot indices remain unchanged and `handguard` is appended as
the fourth slot. An addon is attachable
only when either the weapon or a currently installed parent advertises it. A
parent with installed children cannot be detached; detach the chain from its
leaves first.

## Gameplay classes, required parts and defaults

`addon_slot` names the physical mounting point. `addon_class` independently
names the gameplay role. When `addon_class` is omitted it falls back to
`addon_slot`. Root weapon sections accept arbitrary `<name>_addons` points,
including `sight_rear`, `sight_front`, `scope`, `silencer` and
`grenade_launcher`.

```ini
[wpn_example]
sight_rear_addons       = rear_sight, optic_example
muzzle_addons           = muzzle_brake, suppressor_example
grenade_launcher_addons = launcher_example

critical_addon_classes = magazine, receiver, stock, sight_rear
preinstalled_addons = magazine_default, receiver_default, stock_default, rear_sight

[optic_example]
addon_slot  = sight_rear
addon_class = scope
required_addons = receiver_default
incompatible_addons = rear_sight

; Normal X-Ray scope parameters are read from this section.
scope_zoom_factor   = 20
scope_dynamic_zoom  = true
scope_texture       = scope_example

; Aim offsets are derived from this bone in the installed HUD visual.
aim_from_bone = true
aim_bone       = mod_aim_camera
alt_aim_bone   = mod_alt_aim_camera

; Optional mesh bone carrying a collimator reticle. The HUD copy is visible
; only after ADS has completed through this addon scope; the attachment editor
; preview keeps it visible. The reticle keeps its authored models\collimsight
; material and is depth-tested through the models\transparent lens material.
collimator_bone = reticle

[suppressor_example]
addon_slot  = muzzle
addon_class = muzzle
is_silencer = true

; Normal X-Ray silencer parameters are read from this section.
bullet_hit_power_k     = 0.9
bullet_speed_k         = 0.9
fire_dispersion_base_k = 1.0
cam_dispersion_k       = 0.8

[launcher_example]
addon_slot  = grenade_launcher
addon_class = grenade_launcher
grenade_vel = 76
```

`critical_addon_classes` prevents the weapon from entering any inventory
weapon slot while one of the listed roles is absent. `preinstalled_addons`
installs a comma-separated set on a newly spawned weapon. A single point can
instead use `<slot>_installed = addon_section`.

`required_addons` lists addon section names which must already be installed
before this addon can be attached. A required addon cannot be detached while
an installed addon still depends on it. `incompatible_addons` lists addon
sections which cannot be installed together. The incompatibility check is
bidirectional, so declaring it in either addon section is sufficient. The
legacy misspelling `incopatible_addons` is accepted as an alias.

For an addon with `is_silencer = true`, firing selects
`snd_silncer_shot` and, for the actor when present,
`snd_silncer_shot_actor` from the weapon section. If the silenced sound is
not configured, the normal shot sound is used as a safe fallback.

## ImGui addon transform editor

Open `HUD Editor` and expand `Addon transforms`. The panel lists all currently
installed separately rendered addons, including nested children. `HUD
transform` switches between first-person and world values. Position, rotation
in degrees and scale update live.

`Reset addon transform` removes the runtime override. `Copy config lines`
copies the current mode-specific `hud_attach_*` or `world_attach_*` values for
pasting into the selected addon's config section.

## Addon-authored HUD hand pose

A Source addon such as a handguard or foregrip may include an `idle` motion
that authors the position of one or both hands. Enable that motion in the addon
section:

```ini
[handguard_example]
addon_slot              = handguard
attach_visual           = weapons\addons\handguard_example
hud_hand_pose           = left
hud_hand_pose_animation = idle
hud_hand_pose_blend_in  = 0.18
hud_hand_pose_blend_out = 0.10
hud_hand_pose_ik_time   = 0.78
hud_hand_pose_hold_between_animations = true
# ARC9-style optional timeline: normalized_time:IK_weight
hud_hand_pose_ik_timeline = 0:1, 0.12:0, 0.72:0, 1:1
```

`hud_hand_pose` accepts `left`, `right`, or `both`. The animation defaults to
`idle` when `hud_hand_pose_animation` is omitted. Like ARC9 LHIK/RHIK, the
system evaluates the addon as a hidden pose proxy and blends all matching
ValveBiped arm and finger matrices into the active weapon animation.

`hud_hand_pose_blend_in` and `hud_hand_pose_blend_out` are transition times in
seconds. During reload, draw, fire, inspection and check animations the IK
weight first fades out. `hud_hand_pose_ik_time` is a normalized animation time
from 0 to 1 at which the grip begins fading back in before the action ends; use
`-1` to wait until the weapon returns to idle. A concrete HUD animation can
override it in the weapon HUD section:

```ini
hand_pose_ik_time_anm_reload    = 0.72
hand_pose_ik_time_anm_reload_10 = 0.76
```

For precise ARC9-style control, `hud_hand_pose_ik_timeline` contains comma
separated `normalized_time:weight` stages. Stage interpolation uses ARC9's
InOutQuart + qerp curve. A concrete motion can override the complete timeline
in either the addon or weapon HUD section:

```ini
hand_pose_ik_timeline_anm_reload    = 0:1, 0.08:0, 0.70:0, 1:1
hand_pose_ik_timeline_anm_reload_10 = 0:1, 0.10:0, 0.76:0, 1:1
```

`hud_hand_pose_hide_visual_arms` defaults to `false`. Keep it disabled for an
attachment model which only contains the addon mesh plus pose bones. Scaling
arm bones to zero can stretch mixed-weight triangles; a model containing an
actual visible c_arms mesh should use a separately exported render visual and
pose proxy instead.

`hud_hand_pose_hold_between_animations` defaults to `true` and prevents a
one-frame/state-transition drop to the weapon-authored pose while no timed HUD
motion is active.

Some motions can be driven entirely by the attachment proxy. Their IK weight
is forced to 1, so the weapon's own arm animation cannot pull the hand away:

```ini
[handguard_example]
hud_hand_pose_override_animations = anm_shots, anm_shoot, anm_shoot_aim

# Optional attachment cycles selected while these weapon aliases are active.
# If omitted, hud_hand_pose_animation (normally idle) remains active.
hud_hand_pose_animation_anm_shots = fire
hud_hand_pose_animation_anm_shoot = fire
```

The weapon HUD section can extend/override the same behaviour:

```ini
hand_pose_override_animations = anm_shots, anm_shoot
hand_pose_animation_anm_shots = fire
```

Names in the override list are the actual `anm_*` aliases stored in
`m_current_motion`, not raw OGF cycle names. The mapped value (`fire` above) is
the cycle played on the attachment proxy.

The suffix is the actual `anm_*` alias selected by the weapon. Source addon
exports commonly contain their authoring c_arms geometry; its left and right
arm branches are hidden automatically while their bones remain available for
pose sampling. Set `hud_hand_pose_hide_visual_arms = false` only when that
geometry is intentionally part of the visible addon.

The hand pose follows the addon's complete HUD attachment transform, including
`hud_attach_position`, `hud_attach_rotation`, its attachment space, and anchor
alignment. Removing the addon immediately restores the weapon as the hand pose
source. This feature requires the Source HUD hands/skeleton-merge path; legacy
X-Ray hand rigs continue to ignore addon-authored Source poses.

## Detachable-magazine gameplay

The system is enabled by default and can be toggled from the console:

```text
g_detachable_magazines on
g_detachable_magazines off
```

Only weapons with a non-empty `magazine_addons` list use the detachable-
magazine rules. With the system enabled, such a weapon has capacity `1` while
no magazine is installed. Reload then plays `anm_load_single`, uses
`snd_load_single`, and loads exactly one cartridge. Installing a magazine sets
capacity from its `magazine_capacity`. Removing a magazine safely returns rounds
above the new capacity to the owner instead of deleting them.

`use_reload_animations = 10` selects these keys in the weapon HUD section:

```ini
anm_reload_10       = reload10rnd
anm_reload_10_t     = reload10rndt
anm_reload_10_empty = reload10rnd_empty
```

The matching sound keys live in the normal weapon section. They may instead be
placed in the magazine addon section when a sound belongs only to that addon:

```ini
snd_reload_10       = weapons\example\reload_10
snd_reload_10_t     = weapons\example\reload_10_t
snd_reload_10_empty = weapons\example\reload_10_empty
```

`use_magcheck_animations` is optional. When omitted, it inherits
`use_reload_animations`. For value `10`, the HUD key is `anm_magcheck_10` and
the corresponding sound key is `snd_magcheck_10`. Both fall back to the generic
`anm_magcheck` / `snd_magcheck` keys.

The other interaction animations are configured on the weapon and its HUD:

```ini
; Weapon HUD section
anm_load_single  = load_single
anm_look         = look
anm_bore         = bore
anm_magcheck     = magcheck
anm_muzzle_check = muzzle_check
anm_ready        = ready

; Normal weapon section
snd_load_single  = weapons\example\load_single
snd_look         = weapons\example\look
snd_bore         = weapons\example\bore
snd_magcheck     = weapons\example\magcheck
snd_muzzle_check = weapons\example\muzzle_check
snd_ready        = weapons\example\ready

; Optional random automatic bore delay, in seconds:
bore_idle_time_min = 30
bore_idle_time_max = 60
```

The existing `anim_bore` input action now starts `anm_look`, preserving old
binds. `anm_bore` is automatic and starts after the configured random idle
delay. The additional bindable actions are `anim_magcheck` and
`anim_muzzle_check`. Magazine checking displays the current loaded/capacity
count after its animation. `anm_ready` replaces the usual draw animation on the
first valid draw only; loading a save or taking the weapon from a corpse does
not replay it.

## Bones hidden on empty

The normal weapon section can contain a comma-separated list of bones that are
visible while ammunition is loaded and hidden when the weapon reaches zero:

```ini
[wpn_example]
empty_hide_bones = cartridge, loaded_round, magazine_rounds
```

The list applies to both the world and HUD models. If their bone names differ,
put another `empty_hide_bones` list in the weapon HUD section; that list
overrides the HUD fallback. `hide_bones_when_empty` is accepted as an alias.

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

## Слоты модульного оружия

Фиксированные корневые слоты новой системы:

```ini
stock_addons        = stock_a, stock_b
load_grip_addons    = charging_handle_a
magazine_addons     = magazine_a
muzzle_addons       = muzzle_brake_a, suppressor_a
pistolgrip_addons   = pistolgrip_a
receiver_addons     = receiver_cover_a
gas_block_addons    = gas_block_a
backup_scope_addons = backup_sight_a
```

Соответствующие значения `addon_slot`: `stock`, `load_grip`, `magazine`, `muzzle`, `pistolgrip`, `receiver`, `gas_block` и `backup_scope`. Основной `scope` и `launcher` продолжают использовать штатные слоты X-Ray, поскольку с ними связана отдельная игровая логика прицеливания и подствольного оружия. Старые `foregrip`, `side_rail` и `handguard` оставлены совместимыми с уже созданными конфигами и сохранениями.

Для attach-on-attach имя точки больше не должно быть известно движку. Любая строка `<произвольное_имя>_addons` в секции аттача автоматически создаёт именованную точку именно на этом аттаче:

```ini
[handguard_a]
addon_slot         = gas_block
bottom_rail_addons = foregrip_a, bipod_a
right_rail_addons  = flashlight_a

[foregrip_a]
addon_slot    = bottom_rail
attach_parent = handguard_a

[flashlight_a]
addon_slot    = right_rail
attach_parent = handguard_a
```

Имена `bottom_rail` и `right_rail` здесь полностью конфиговые. Одинаковое имя на двух разных родительских аттачах создаёт две независимые точки. Одновременно доступно до 13 произвольных точек на одном дереве оружия; фиксированные слоты в этот лимит не входят. Снимать дерево нужно от листьев к корню.
# Координатное опускание оружия при спринте

Параметры задаются в HUD-секции оружия (той, которая указана в строке `hud = ...`):

```ini
sprint_hud_offset_pos       = 0.0, -0.12, -0.04
sprint_hud_offset_rot       = -15.0, 0.0, 0.0
sprint_hud_offset_time      = 0.25
sprint_hud_offset_enabled   = true
```

Поворот задаётся в градусах. `sprint_hud_offset_time` — время плавного входа и выхода в секундах. Если присутствует хотя бы `sprint_hud_offset_pos` или `sprint_hud_offset_rot`, режим включается автоматически; строка `sprint_hud_offset_enabled` нужна только для явного включения или отключения. Поддерживаются варианты координат с суффиксом `_16x9`.

При включённом координатном режиме `anm_idle_sprint_start`, `anm_idle_sprint` и `anm_idle_sprint_end` не проигрываются: используется обычный idle, поверх которого плавно накладывается указанное смещение. Спринтовый movement layer продолжает работать и смешивается с координатной позой.

Инерция направления во время этой процедурной спринтовой позы не накладывается: её внутреннее направление синхронизируется с камерой. Поэтому оружие не накапливает боковой сдвиг в долгом спринте и не дёргается при выходе из него.

## Отдача ARC9

`cam_recoil_modern = true` включает перенесённую модель ARC9: паттерн с дрейфом, случайные вертикальную и боковую составляющие, 30-миллисекундный импульс камеры, автокомпенсацию и нелинейную velocity-Verlet пружину HUD. Параметры имеют префикс `arc9_`; для Source-моделей обычно используется `arc9_visual_recoil_scale = 0.025`.

Основные параметры камеры: `arc9_recoil`, `arc9_recoil_up`, `arc9_recoil_side`, `arc9_recoil_random_up`, `arc9_recoil_random_side`, `arc9_recoil_auto_control`, `arc9_recoil_pattern_drift`, `arc9_recoil_reset_time` и `arc9_recoil_full_reset_time`.

Для адаптации исходного 30-миллисекундного импульса ARC9 к камере X-Ray предусмотрены `arc9_camera_recoil_scale` и `arc9_camera_impulse_time`. Меньший scale ослабляет камеру, большее время делает толчок мягче, не меняя паттерн.

Визуальная отдача настраивается через `arc9_visual_recoil_*`: отдельные значения `up/side` для первых выстрелов и полного автомата, `roll`, `punch`, центр вращения, масштаб и три параметра пружины. `arc9_subtle_visual_recoil*` включает малую высокочастотную составляющую ARC9.
