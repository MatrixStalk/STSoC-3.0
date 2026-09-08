# HUD root inertia

Every `CHudItem` now applies camera-direction inertia to the common HUD root,
before the hands attachment and item transform are evaluated. As a result,
the hands, weapon or other held item, and its attached models pitch and turn as
one assembly. The existing positional sway remains supported.

The defaults enable a restrained rotational response for existing HUD sections.
They can be overridden per HUD section:

```ini
[wpn_example_hud]
allow_inertion                   = true

; Existing positional inertia.
inertion_origin_offset           = -0.03
inertion_zoom_origin_offset      = -0.02
inertion_tendto_speed            = 5.0
inertion_zoom_tendto_speed       = 10.0

; New common-root rotation. Factors are multipliers; the limit is in degrees.
inertion_rotation_pitch_factor   = 0.70
inertion_rotation_yaw_factor     = 0.45
inertion_rotation_roll_factor    = 0.18
inertion_rotation_limit          = 6.0
inertion_rotation_zoom_factor    = 0.35
```

`inertion_rotation_pitch_factor` is the main vertical camera-lag effect.
`inertion_rotation_yaw_factor` turns the HUD toward its delayed direction, and
`inertion_rotation_roll_factor` adds a small bank during horizontal turns.
Negative factor values invert an axis. `inertion_rotation_zoom_factor` is the
fraction of the rotational effect retained at full aim; the transition follows
the normal HUD aiming blend rather than switching instantly.

The direction response uses exponential interpolation, so it is stable across
frame rates and does not overshoot after a frame-time spike. Attaching or
detaching an item resets its history to prevent a stale-direction kick. During
a procedural sprint pose, only the bounded root rotation is kept; positional
sway is suppressed to avoid cumulative drift.
