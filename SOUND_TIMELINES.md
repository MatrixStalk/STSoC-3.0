# Sound timelines and attachment overrides

The weapon sound loader accepts the legacy single-sound value and a timeline
section. Values in a timeline keep the existing order:

`sound, volume, delay_seconds, pitch`

## Animation example

This timing layout is adapted from the supplied TarkovARS G36 ARC9 sound
tables (`13/24 - 0.1` for magazine out and `61/24 - 0.2` for magazine in):

```ini
[wpn_example]
snd_anm_anm_reload = wpn_example_reload_timeline
animation_sounds_replace_legacy = true

[wpn_example_reload_timeline]
layers = mag_out, mag_in, bolt
layer_mag_out = weapons\example\g36_mag_out, 1.0, 0.4417, 1.0
layer_mag_in  = weapons\example\g36_mag_in,  1.0, 2.3417, 1.0
layer_bolt    = weapons\example\g36_bolt,    0.9, 2.6500, 1.0
```

`snd_anm_<actual_hud_motion>` is evaluated after animation fallbacks have been
resolved. This makes it possible to give every concrete animation its own
timeline. Set `animation_sounds_replace_legacy = true` to silence the old
hard-coded action sounds on that weapon; omit it to use animation timelines as
additional layers.

The old layered format remains valid:

```ini
[wpn_example_reload_timeline_legacy]
snd_1_layer = weapons\example\mag_out, 1.0, 0.55, 1.0
snd_2_layer = weapons\example\mag_in,  1.0, 1.82, 1.0
```

## Override from any installed attachment

Every `snd_*` key is resolved again against installed attachments at playback
time. The attachment with the greatest `sound_override_priority` wins.

```ini
[mag_example_metal]
sound_override_priority = 20
snd_reload               = mag_example_reload_override
snd_reload_empty         = mag_example_reload_empty_override
snd_anm_anm_reload       = mag_example_reload_override

; LTX inheritance permits replacing one named layer only.
[mag_example_reload_override]:wpn_example_reload_timeline
layer_mag_in = weapons\example\mag_in_metal, 1.0, 1.82, 1.0
```

Use `snd_reload = none` (or any other `snd_* = none`) to suppress a legacy
sound while that attachment is installed.

## Indoor/outdoor shots

```ini
[wpn_example]
snd_shoot                       = wpn_example_shot_outdoor
snd_shoot_actor                 = wpn_example_shot_outdoor_actor
snd_shoot_indoor                = wpn_example_shot_indoor
snd_shoot_actor_indoor          = wpn_example_shot_indoor_actor
snd_silncer_shot                = wpn_example_suppressed_outdoor
snd_silncer_shot_indoor         = wpn_example_suppressed_indoor
indoor_sound_check_distance     = 30.0
```

The listener is considered indoors when static geometry is found above the
muzzle within `indoor_sound_check_distance`. Missing indoor definitions fall
back to outdoor ones.

## ImGui tools

Open **SoundEnv** in the embedded editor. **Animation sound timeline** scans
all `.ogg` files below `$game_sounds$`, lets you add named layers and edit their
volume, pitch and start time, and saves an LTX section. The default
`config\sound_timelines.ltx` is loaded directly by the weapon sound system and
is refreshed immediately after saving, so it does not need a `system.ltx`
include. The same window contains the live four-band level equalizer and
Save/Reload controls for `config\sound_equalizer.ltx`.
